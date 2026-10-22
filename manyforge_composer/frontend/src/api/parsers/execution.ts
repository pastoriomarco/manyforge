import type { ProgramNodeCorrectionPreview, ProgramNodeCorrectionPreviewRequest } from '../../types/api';
import type {
  CapabilityCommandPreviewReceipt,
  CoordinateKind,
  DirectControlJoint,
  ManualControlContracts,
  ManualControlGripperDescriptor,
  ManualControlMotionDescriptor,
  DirectControlGripperStatus,
  DirectControlMotionStatus,
  ExecutionComponentStatus,
  ExecutionIdentity,
  ExecutionOwnerStatus,
  MotionPreviewReceipt,
  ObservedGripperTarget,
  ObservedMotionTarget,
  OperationObservation,
  OperationStateName,
  OutputEvidence,
  ResourceSourceKind,
  SceneRevisionValue,
  TargetStopResponse,
  ManualControlAvailability,
  ManualProgramHandoverContext,
} from '../../types/robotControl';
import type { JsonObject } from './shared';
import { ioSignalBindingsAt } from './ioSignals';
import {
  allowedKeysAt,
  arrayAt,
  booleanAt,
  boundedArrayAt,
  digestAt,
  exactKeysAt,
  fail,
  integerAt,
  nonEmptyStringAt,
  nullableNumberAt,
  nullableStringAt,
  numberAt,
  objectAt,
  positiveIntegerAt,
  positiveNumberAt,
  stringArrayAt,
  stringAt,
} from './shared';
import { parseApiErrorDto, runtimeScaleControlStatusAt } from './programRuntime';

function sceneRevisionAt(value: unknown, path: string): SceneRevisionValue {
  const record = objectAt(value, path);
  const revisionId = integerAt(record.revision_id, `${path}.revision_id`);
  if (revisionId < 1) return fail(`${path}.revision_id`, 'positive');
  return {
    scene_id: stringAt(record.scene_id, `${path}.scene_id`),
    revision_id: revisionId,
    content_hash: stringAt(record.content_hash, `${path}.content_hash`),
  };
}

function directControlJointAt(
  value: unknown,
  path: string,
): DirectControlJoint {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'max_acceleration_rad_s2',
    'max_position_rad',
    'max_velocity_rad_s',
    'min_position_rad',
    'source_joint_name',
  ], path);
  return {
    sourceJointName: stringAt(
      record.source_joint_name,
      `${path}.source_joint_name`,
    ),
    minPositionRad: numberAt(
      record.min_position_rad,
      `${path}.min_position_rad`,
    ),
    maxPositionRad: numberAt(
      record.max_position_rad,
      `${path}.max_position_rad`,
    ),
    maxVelocityRadS: numberAt(
      record.max_velocity_rad_s,
      `${path}.max_velocity_rad_s`,
    ),
    maxAccelerationRadS2: nullableNumberAt(
      record.max_acceleration_rad_s2,
      `${path}.max_acceleration_rad_s2`,
    ),
  };
}

function manualControlContractsAt(
  value: unknown,
  path: string,
): ManualControlContracts {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'command_schema', 'control_path',
    'operation_capability_definition_revision', 'operation_capability_id',
    'supporting_providers',
  ], path);
  const commandSchema = record.command_schema === null ? null : (() => {
    const schema = objectAt(record.command_schema, `${path}.command_schema`);
    exactKeysAt(schema, ['command_schema_id', 'command_schema_revision'], `${path}.command_schema`);
    return {
      commandSchemaId: stringAt(schema.command_schema_id, `${path}.command_schema.command_schema_id`),
      commandSchemaRevision: digestAt(schema.command_schema_revision, `${path}.command_schema.command_schema_revision`),
    };
  })();
  const controlPath = record.control_path === null ? null : (() => {
    const control = objectAt(record.control_path, `${path}.control_path`);
    exactKeysAt(control, [
      'control_binding_id', 'control_binding_revision',
      'control_interface_contract_id', 'control_interface_revision',
      'controller_topology_id', 'stop_evidence_policy_id',
      'stop_evidence_policy_revision', 'topology_revision',
    ], `${path}.control_path`);
    return {
      controlBindingId: stringAt(control.control_binding_id, `${path}.control_path.control_binding_id`),
      controlBindingRevision: digestAt(control.control_binding_revision, `${path}.control_path.control_binding_revision`),
      controlInterfaceContractId: stringAt(control.control_interface_contract_id, `${path}.control_path.control_interface_contract_id`),
      controlInterfaceRevision: digestAt(control.control_interface_revision, `${path}.control_path.control_interface_revision`),
      controllerTopologyId: stringAt(control.controller_topology_id, `${path}.control_path.controller_topology_id`),
      topologyRevision: digestAt(control.topology_revision, `${path}.control_path.topology_revision`),
      stopEvidencePolicyId: stringAt(control.stop_evidence_policy_id, `${path}.control_path.stop_evidence_policy_id`),
      stopEvidencePolicyRevision: digestAt(control.stop_evidence_policy_revision, `${path}.control_path.stop_evidence_policy_revision`),
    };
  })();
  const supportingProviders = boundedArrayAt(
    record.supporting_providers,
    `${path}.supporting_providers`,
    64,
  ).map((item, index) => {
    const itemPath = `${path}.supporting_providers[${index}]`;
    const provider = objectAt(item, itemPath);
    exactKeysAt(provider, [
      'capability_definition_revision', 'capability_id',
      'capability_provider_binding_id', 'capability_provider_binding_revision',
      'provider_manifest_id', 'provider_manifest_revision',
    ], itemPath);
    return {
      capabilityId: stringAt(provider.capability_id, `${itemPath}.capability_id`),
      capabilityDefinitionRevision: digestAt(provider.capability_definition_revision, `${itemPath}.capability_definition_revision`),
      capabilityProviderBindingId: stringAt(provider.capability_provider_binding_id, `${itemPath}.capability_provider_binding_id`),
      capabilityProviderBindingRevision: digestAt(provider.capability_provider_binding_revision, `${itemPath}.capability_provider_binding_revision`),
      providerManifestId: stringAt(provider.provider_manifest_id, `${itemPath}.provider_manifest_id`),
      providerManifestRevision: digestAt(provider.provider_manifest_revision, `${itemPath}.provider_manifest_revision`),
    };
  });
  if (new Set(supportingProviders.map(
    (provider) => provider.capabilityProviderBindingId,
  )).size !== supportingProviders.length) {
    return fail(`${path}.supporting_providers`, 'unique provider bindings');
  }
  return {
    operationCapabilityId: stringAt(record.operation_capability_id, `${path}.operation_capability_id`),
    operationCapabilityDefinitionRevision: digestAt(record.operation_capability_definition_revision, `${path}.operation_capability_definition_revision`),
    commandSchema,
    controlPath,
    supportingProviders,
  };
}

function manualControlMotionDescriptorAt(
  value: unknown,
  path: string,
): ManualControlMotionDescriptor {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'command_target_id', 'component_role_id', 'contracts',
    'controllable_component_id', 'frames', 'joints', 'operational_joint_limits',
    'operational_limit', 'ordinary_jerk', 'schema_version', 'target_role_id',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.manual_control_motion_descriptor.v5') {
    return fail(`${path}.schema_version`, 'the supported motion descriptor');
  }
  const operationalLimit = record.operational_limit === null
    ? null
    : (() => {
        const limitPath = `${path}.operational_limit`;
        const limit = objectAt(record.operational_limit, limitPath);
        exactKeysAt(limit, ['maximum_tcp_linear_speed_mps'], limitPath);
        return {
          maximumTcpLinearSpeedMps: positiveNumberAt(
            limit.maximum_tcp_linear_speed_mps,
            `${limitPath}.maximum_tcp_linear_speed_mps`,
          ),
        };
      })();
  const frames = boundedArrayAt(record.frames, `${path}.frames`, 256).map((item, index) => {
    const itemPath = `${path}.frames[${index}]`;
    const frame = objectAt(item, itemPath);
    exactKeysAt(frame, ['compiled_frame_name', 'frame_role_id', 'model_instance_id', 'model_revision'], itemPath);
    return {
      compiledFrameName: stringAt(frame.compiled_frame_name, `${itemPath}.compiled_frame_name`),
      frameRoleId: stringAt(frame.frame_role_id, `${itemPath}.frame_role_id`),
      modelInstanceId: stringAt(frame.model_instance_id, `${itemPath}.model_instance_id`),
      modelRevision: digestAt(frame.model_revision, `${itemPath}.model_revision`),
    };
  });
  if (
    frames.length === 0
    || new Set(frames.map((frame) => frame.frameRoleId)).size !== frames.length
  ) {
    return fail(`${path}.frames`, 'non-empty unique frame roles');
  }
  const joints = boundedArrayAt(record.joints, `${path}.joints`, 1024).map((item, index) => {
    const itemPath = `${path}.joints[${index}]`;
    const joint = objectAt(item, itemPath);
    exactKeysAt(joint, [
      'compiled_joint_name', 'max_acceleration_rad_s2', 'max_position_rad',
      'max_velocity_rad_s', 'min_position_rad', 'source_joint_name', 'unit',
    ], itemPath);
    if (stringAt(joint.unit, `${itemPath}.unit`) !== 'rad') {
      return fail(`${itemPath}.unit`, 'rad');
    }
    return {
      compiledJointName: stringAt(joint.compiled_joint_name, `${itemPath}.compiled_joint_name`),
      sourceJointName: stringAt(joint.source_joint_name, `${itemPath}.source_joint_name`),
      minPositionRad: numberAt(joint.min_position_rad, `${itemPath}.min_position_rad`),
      maxPositionRad: numberAt(joint.max_position_rad, `${itemPath}.max_position_rad`),
      maxVelocityRadS: positiveNumberAt(joint.max_velocity_rad_s, `${itemPath}.max_velocity_rad_s`),
      maxAccelerationRadS2: nullableNumberAt(joint.max_acceleration_rad_s2, `${itemPath}.max_acceleration_rad_s2`),
      unit: 'rad' as const,
    };
  });
  if (
    joints.length === 0
    || new Set(joints.map((joint) => joint.sourceJointName)).size !== joints.length
    || new Set(joints.map((joint) => joint.compiledJointName)).size !== joints.length
  ) {
    return fail(`${path}.joints`, 'non-empty unique source and compiled joints');
  }
  for (const [index, joint] of joints.entries()) {
    if (
      joint.minPositionRad > joint.maxPositionRad
      || joint.maxVelocityRadS <= 0
      || (joint.maxAccelerationRadS2 !== null && joint.maxAccelerationRadS2 <= 0)
    ) {
      return fail(`${path}.joints[${index}]`, 'ordered finite limits');
    }
  }
  const ordinaryJerk: ManualControlMotionDescriptor['ordinaryJerk'] = record.ordinary_jerk === null
    ? null : (() => {
      const base = `${path}.ordinary_jerk`;
      const profile = objectAt(record.ordinary_jerk, base);
      exactKeysAt(profile, ['enabled', 'preset_reference_time_s', 'joint_limits'], base);
      const enabled = booleanAt(profile.enabled, `${base}.enabled`);
      const jointLimits = boundedArrayAt(profile.joint_limits, `${base}.joint_limits`, 1024)
        .map((value, index) => {
          const itemPath = `${base}.joint_limits[${index}]`;
          const row = objectAt(value, itemPath);
          exactKeysAt(row, ['source_joint_name', 'source', 'maximum_rad_s3', 'preset_rad_s3', 'imported'], itemPath);
          const source = stringAt(row.source, `${itemPath}.source`);
          if (!['off', 'override', 'imported', 'preset'].includes(source)) {
            return fail(itemPath, 'a known jerk-limit source');
          }
          const maximumRadS3 = nullableNumberAt(row.maximum_rad_s3, `${itemPath}.maximum_rad_s3`);
          const presetRadS3 = nullableNumberAt(row.preset_rad_s3, `${itemPath}.preset_rad_s3`);
          const imported = row.imported === null ? null : (() => {
            const importedPath = `${itemPath}.imported`;
            const info = objectAt(row.imported, importedPath);
            exactKeysAt(info, ['maximum_rad_s3', 'enabled', 'machine_required', 'source_id', 'source_digest'], importedPath);
            return {
              maximumRadS3: positiveNumberAt(info.maximum_rad_s3, `${importedPath}.maximum_rad_s3`),
              enabled: booleanAt(info.enabled, `${importedPath}.enabled`),
              machineRequired: booleanAt(info.machine_required, `${importedPath}.machine_required`),
              sourceId: stringAt(info.source_id, `${importedPath}.source_id`),
              sourceDigest: digestAt(info.source_digest, `${importedPath}.source_digest`),
            };
          })();
          if ((enabled ? maximumRadS3 === null || source === 'off' : maximumRadS3 !== null || source !== 'off')
            || (maximumRadS3 !== null && maximumRadS3 <= 0)
            || (presetRadS3 !== null && presetRadS3 <= 0)
            || (source === 'preset' && maximumRadS3 !== presetRadS3)
            || (source === 'imported' && (!imported?.enabled || maximumRadS3 !== imported.maximumRadS3))
            || (imported?.enabled && maximumRadS3 !== null && maximumRadS3 > imported.maximumRadS3)
            || (imported?.machineRequired && (!enabled || !imported.enabled))) {
            return fail(itemPath, 'consistent effective jerk limits and provenance');
          }
          return {
            sourceJointName: stringAt(row.source_joint_name, `${itemPath}.source_joint_name`),
            source: source as 'off' | 'override' | 'imported' | 'preset',
            maximumRadS3, presetRadS3, imported,
          };
        });
      if (jointLimits.length !== joints.length || jointLimits.some((row, index) => row.sourceJointName !== joints[index].sourceJointName)) {
        return fail(base, 'jerk limits joined to the exact ordered model joints');
      }
      return { enabled, presetReferenceTimeS: positiveNumberAt(profile.preset_reference_time_s, `${base}.preset_reference_time_s`), jointLimits };
    })();
  const operationalJointLimits = boundedArrayAt(
    record.operational_joint_limits,
    `${path}.operational_joint_limits`,
    1024,
  ).map((item, index) => {
    const itemPath = `${path}.operational_joint_limits[${index}]`;
    const limit = objectAt(item, itemPath);
    exactKeysAt(limit, [
      'deployment_max_acceleration_rad_s2', 'deployment_max_jerk_rad_s3',
      'deployment_max_velocity_rad_s', 'effective_max_acceleration_rad_s2',
      'effective_max_jerk_rad_s3', 'effective_max_velocity_rad_s',
      'model_max_acceleration_rad_s2', 'model_max_velocity_rad_s',
      'source_joint_name',
    ], itemPath);
    const result = {
      sourceJointName: stringAt(limit.source_joint_name, `${itemPath}.source_joint_name`),
      modelMaxVelocityRadS: positiveNumberAt(
        limit.model_max_velocity_rad_s,
        `${itemPath}.model_max_velocity_rad_s`,
      ),
      modelMaxAccelerationRadS2: nullableNumberAt(
        limit.model_max_acceleration_rad_s2,
        `${itemPath}.model_max_acceleration_rad_s2`,
      ),
      deploymentMaxVelocityRadS: nullableNumberAt(
        limit.deployment_max_velocity_rad_s,
        `${itemPath}.deployment_max_velocity_rad_s`,
      ),
      deploymentMaxAccelerationRadS2: nullableNumberAt(
        limit.deployment_max_acceleration_rad_s2,
        `${itemPath}.deployment_max_acceleration_rad_s2`,
      ),
      deploymentMaxJerkRadS3: nullableNumberAt(
        limit.deployment_max_jerk_rad_s3,
        `${itemPath}.deployment_max_jerk_rad_s3`,
      ),
      effectiveMaxVelocityRadS: positiveNumberAt(
        limit.effective_max_velocity_rad_s,
        `${itemPath}.effective_max_velocity_rad_s`,
      ),
      effectiveMaxAccelerationRadS2: nullableNumberAt(
        limit.effective_max_acceleration_rad_s2,
        `${itemPath}.effective_max_acceleration_rad_s2`,
      ),
      effectiveMaxJerkRadS3: nullableNumberAt(
        limit.effective_max_jerk_rad_s3,
        `${itemPath}.effective_max_jerk_rad_s3`,
      ),
    };
    for (const [name, value] of Object.entries(result)) {
      if (name !== 'sourceJointName' && value !== null && typeof value === 'number' && value <= 0) {
        return fail(itemPath, 'positive dynamics limits');
      }
    }
    const expectedVelocity = result.deploymentMaxVelocityRadS === null
      ? result.modelMaxVelocityRadS
      : Math.min(result.modelMaxVelocityRadS, result.deploymentMaxVelocityRadS);
    const expectedAcceleration = result.modelMaxAccelerationRadS2 === null
      ? result.deploymentMaxAccelerationRadS2
      : result.deploymentMaxAccelerationRadS2 === null
        ? result.modelMaxAccelerationRadS2
        : Math.min(
            result.modelMaxAccelerationRadS2,
            result.deploymentMaxAccelerationRadS2,
          );
    if (
      result.effectiveMaxVelocityRadS !== expectedVelocity
      || result.effectiveMaxAccelerationRadS2 !== expectedAcceleration
      || result.effectiveMaxJerkRadS3 !== (ordinaryJerk === null
        ? result.deploymentMaxJerkRadS3
        : !ordinaryJerk.enabled ? null
          : Math.min(result.deploymentMaxJerkRadS3 ?? Infinity,
            ordinaryJerk.jointLimits[index].maximumRadS3!))
    ) {
      return fail(itemPath, 'the exact conservative Deployment/model minimum');
    }
    return result;
  });
  if (
    operationalJointLimits.length !== joints.length
    || operationalJointLimits.some((limit, index) => (
      limit.sourceJointName !== joints[index]?.sourceJointName
      || limit.modelMaxVelocityRadS !== joints[index]?.maxVelocityRadS
      || limit.modelMaxAccelerationRadS2 !== joints[index]?.maxAccelerationRadS2
    ))
  ) {
    return fail(`${path}.operational_joint_limits`, 'limits joined conservatively to each source joint');
  }
  return {
    schemaVersion,
    controllableComponentId: stringAt(record.controllable_component_id, `${path}.controllable_component_id`),
    commandTargetId: stringAt(record.command_target_id, `${path}.command_target_id`),
    componentRoleId: stringAt(record.component_role_id, `${path}.component_role_id`),
    targetRoleId: stringAt(record.target_role_id, `${path}.target_role_id`),
    frames,
    joints,
    operationalJointLimits,
    ordinaryJerk,
    operationalLimit,
    contracts: manualControlContractsAt(record.contracts, `${path}.contracts`),
  };
}

function manualControlGripperDescriptorAt(
  value: unknown,
  path: string,
): ManualControlGripperDescriptor {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'actuation_kind', 'attach_frame', 'auxiliary_frames', 'closed_state_ref',
    'command_duration_s', 'command_kind', 'command_target_id',
    'component_role_id', 'completion_policy', 'contracts',
    'control_interface_contract_id', 'controllable_component_id',
    'coordinate_kind', 'description', 'grasp_confirmation_policy',
    'max_position', 'mimic_followers', 'min_position', 'mount_frame',
    'named_presets', 'open_state_ref', 'output_mode', 'schema_version',
    'source_coordinate_name', 'source_group_name', 'source_kind',
    'state_authority_class', 'state_authority_id', 'target_role_id',
    'tcp_frame', 'touch_links', 'transport_kind', 'unit',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.manual_control_gripper_descriptor.v3') {
    return fail(`${path}.schema_version`, 'the supported gripper descriptor');
  }
  const coordinateKind = stringAt(record.coordinate_kind, `${path}.coordinate_kind`);
  if (coordinateKind !== 'angular_rad' && coordinateKind !== 'linear_m') {
    return fail(`${path}.coordinate_kind`, 'angular_rad or linear_m');
  }
  const unit = stringAt(record.unit, `${path}.unit`);
  if ((coordinateKind === 'angular_rad' && unit !== 'rad') || (coordinateKind === 'linear_m' && unit !== 'm')) {
    return fail(`${path}.unit`, 'the coordinate-kind unit');
  }
  const sourceKind = stringAt(record.source_kind, `${path}.source_kind`);
  if (sourceKind !== 'model_role' && sourceKind !== 'component_local') {
    return fail(`${path}.source_kind`, 'model_role or component_local');
  }
  const mimicFollowers = boundedArrayAt(record.mimic_followers, `${path}.mimic_followers`, 1024).map((item, index) => {
    const itemPath = `${path}.mimic_followers[${index}]`;
    const follower = objectAt(item, itemPath);
    exactKeysAt(follower, ['compiled_coordinate_name', 'multiplier', 'offset', 'source_coordinate_name'], itemPath);
    return {
      compiledCoordinateName: stringAt(follower.compiled_coordinate_name, `${itemPath}.compiled_coordinate_name`),
      sourceCoordinateName: stringAt(follower.source_coordinate_name, `${itemPath}.source_coordinate_name`),
      multiplier: numberAt(follower.multiplier, `${itemPath}.multiplier`),
      offset: numberAt(follower.offset, `${itemPath}.offset`),
    };
  });
  const namedPresets = boundedArrayAt(record.named_presets, `${path}.named_presets`, 1024).map((item, index) => {
    const itemPath = `${path}.named_presets[${index}]`;
    const preset = objectAt(item, itemPath);
    exactKeysAt(preset, ['coordinates', 'master_position', 'name'], itemPath);
    return {
      name: stringAt(preset.name, `${itemPath}.name`),
      masterPosition: numberAt(preset.master_position, `${itemPath}.master_position`),
      coordinates: boundedArrayAt(preset.coordinates, `${itemPath}.coordinates`, 1024).map((coordinate, coordinateIndex) => {
        const coordinatePath = `${itemPath}.coordinates[${coordinateIndex}]`;
        const row = objectAt(coordinate, coordinatePath);
        exactKeysAt(row, ['position', 'source_coordinate_name'], coordinatePath);
        return {
          sourceCoordinateName: stringAt(row.source_coordinate_name, `${coordinatePath}.source_coordinate_name`),
          position: numberAt(row.position, `${coordinatePath}.position`),
        };
      }),
    };
  });
  const minPosition = nullableNumberAt(record.min_position, `${path}.min_position`);
  const maxPosition = nullableNumberAt(record.max_position, `${path}.max_position`);
  const description = nullableStringAt(record.description, `${path}.description`);
  const actuationKind = nullableStringAt(record.actuation_kind, `${path}.actuation_kind`);
  const outputMode = nullableStringAt(record.output_mode, `${path}.output_mode`);
  const commandDurationS = nullableNumberAt(record.command_duration_s, `${path}.command_duration_s`);
  const openStateRef = nullableStringAt(record.open_state_ref, `${path}.open_state_ref`);
  const closedStateRef = nullableStringAt(record.closed_state_ref, `${path}.closed_state_ref`);
  const sourceGroupName = nullableStringAt(record.source_group_name, `${path}.source_group_name`);
  const mountFrame = nullableStringAt(record.mount_frame, `${path}.mount_frame`);
  const attachFrame = nullableStringAt(record.attach_frame, `${path}.attach_frame`);
  const tcpFrame = nullableStringAt(record.tcp_frame, `${path}.tcp_frame`);
  const controlInterfaceContractId = nullableStringAt(
    record.control_interface_contract_id,
    `${path}.control_interface_contract_id`,
  );
  const stateAuthorityId = nullableStringAt(record.state_authority_id, `${path}.state_authority_id`);
  const stateAuthorityClass = nullableStringAt(
    record.state_authority_class,
    `${path}.state_authority_class`,
  );
  const strings = (raw: unknown, field: string) => boundedArrayAt(
    raw,
    `${path}.${field}`,
    256,
  ).map((item, index) => {
    const result = stringAt(item, `${path}.${field}[${index}]`);
    if (result.trim() === '') return fail(`${path}.${field}[${index}]`, 'a non-empty source name');
    return result;
  });
  const touchLinks = strings(record.touch_links, 'touch_links');
  const auxiliaryFrames = strings(record.auxiliary_frames, 'auxiliary_frames');
  const contracts = manualControlContractsAt(record.contracts, `${path}.contracts`);
  const commandKind = stringAt(record.command_kind, `${path}.command_kind`);
  const transportKind = stringAt(record.transport_kind, `${path}.transport_kind`);
  const completionPolicy = stringAt(record.completion_policy, `${path}.completion_policy`);
  const graspConfirmationPolicy = stringAt(
    record.grasp_confirmation_policy,
    `${path}.grasp_confirmation_policy`,
  );
  const qualifiedPolicies = actuationKind === 'pneumatic'
    ? graspConfirmationPolicy === 'not_required'
      && ((outputMode === 'commanded' && completionPolicy === 'output_delivery')
        || (outputMode === 'none' && completionPolicy === 'no_output'))
    : completionPolicy === 'goal_fenced_target_reached'
      && graspConfirmationPolicy === 'binding_feedback_required'
      && outputMode !== 'none';
  if (
    commandKind !== 'gripper_position'
    || transportKind !== 'resolved_binding'
    || !qualifiedPolicies
    || (outputMode !== null && outputMode !== 'commanded' && outputMode !== 'none')
    || (actuationKind !== null
      && actuationKind !== 'position_commanded'
      && actuationKind !== 'pneumatic')
    || (stateAuthorityClass !== null
      && stateAuthorityClass !== 'measured'
      && stateAuthorityClass !== 'commanded'
      && stateAuthorityClass !== 'predicted')
  ) {
    return fail(path, 'the exact qualified gripper command semantics');
  }
  const authoredValues = [
    description, actuationKind, outputMode, commandDurationS, openStateRef, closedStateRef,
    mountFrame, attachFrame, tcpFrame,
  ];
  // A retained SRDF group and its presets can exist without authored tool
  // configuration. Once configured, the complete configuration needs both.
  const configured = authoredValues.some((item) => item !== null)
    || touchLinks.length > 0
    || auxiliaryFrames.length > 0;
  if (
    (configured && (
      authoredValues.some((item) => item === null)
      || sourceGroupName === null
      || description?.trim() === ''
      || commandDurationS === null
      || commandDurationS < 0
      || (actuationKind !== 'pneumatic' && commandDurationS === 0)
      || commandDurationS > 60
      || touchLinks.length === 0
      || !namedPresets.some((preset) => preset.name === openStateRef)
      || !namedPresets.some((preset) => preset.name === closedStateRef)
    ))
    || new Set(touchLinks).size !== touchLinks.length
    || new Set(auxiliaryFrames).size !== auxiliaryFrames.length
    || (stateAuthorityId === null) !== (stateAuthorityClass === null)
    || controlInterfaceContractId !== (contracts.controlPath?.controlInterfaceContractId ?? null)
  ) {
    return fail(path, 'one complete, internally consistent gripper configuration');
  }
  if (
    (sourceKind === 'model_role'
      && (minPosition === null || maxPosition === null || minPosition > maxPosition))
    || (sourceKind === 'component_local'
      && (minPosition !== null || maxPosition !== null))
  ) {
    return fail(
      path,
      sourceKind === 'model_role'
        ? 'ordered model-backed limits'
        : 'no fabricated limits for a component-local coordinate',
    );
  }
  if (
    new Set(mimicFollowers.map(
      (follower) => follower.compiledCoordinateName,
    )).size !== mimicFollowers.length
    || new Set(namedPresets.map((preset) => preset.name)).size
      !== namedPresets.length
    || namedPresets.some((preset) => (
      new Set(preset.coordinates.map(
        (coordinate) => coordinate.sourceCoordinateName,
      )).size !== preset.coordinates.length
      || (minPosition !== null && preset.masterPosition < minPosition)
      || (maxPosition !== null && preset.masterPosition > maxPosition)
    ))
  ) {
    return fail(
      `${path}.named_presets`,
      'unique bounded presets, coordinates, and mimic followers',
    );
  }
  return {
    schemaVersion,
    controllableComponentId: stringAt(record.controllable_component_id, `${path}.controllable_component_id`),
    commandTargetId: stringAt(record.command_target_id, `${path}.command_target_id`),
    componentRoleId: stringAt(record.component_role_id, `${path}.component_role_id`),
    targetRoleId: stringAt(record.target_role_id, `${path}.target_role_id`),
    description,
    commandKind,
    actuationKind: actuationKind as ManualControlGripperDescriptor['actuationKind'],
    outputMode: outputMode as ManualControlGripperDescriptor['outputMode'],
    transportKind,
    controlInterfaceContractId,
    stateAuthorityId,
    stateAuthorityClass: stateAuthorityClass as ManualControlGripperDescriptor['stateAuthorityClass'],
    completionPolicy: completionPolicy as ManualControlGripperDescriptor['completionPolicy'],
    graspConfirmationPolicy: graspConfirmationPolicy as ManualControlGripperDescriptor['graspConfirmationPolicy'],
    sourceGroupName,
    sourceCoordinateName: stringAt(record.source_coordinate_name, `${path}.source_coordinate_name`),
    sourceKind: sourceKind as ResourceSourceKind,
    coordinateKind: coordinateKind as CoordinateKind,
    unit: unit as 'rad' | 'm',
    minPosition,
    maxPosition,
    openStateRef,
    closedStateRef,
    commandDurationS,
    touchLinks,
    mountFrame,
    attachFrame,
    tcpFrame,
    auxiliaryFrames,
    mimicFollowers,
    namedPresets,
    contracts,
  };
}

function executionIdentityAt(value: unknown, path: string): ExecutionIdentity {
  // execution_identity.v2 (H1a-IF.2): runtime identity only. No primary
  // target roles and no Program fields: a manual target's direct-control
  // binding lives in that target, the authored Program outcome elsewhere.
  const identity = objectAt(value, path);
  exactKeysAt(identity, [
    'active_publication_generation',
    'cell_id',
    'control_management_mode',
    'graph_revision',
    'owner_epoch',
    'retained_bundle_id',
    'scene_revision',
    'schema_version',
  ], path);
  if (stringAt(identity.schema_version, `${path}.schema_version`)
      !== 'manyforge.execution_identity.v2') {
    return fail(`${path}.schema_version`, 'manyforge.execution_identity.v2');
  }
  const controlManagementMode = stringAt(
    identity.control_management_mode,
    `${path}.control_management_mode`,
  );
  if (!['pending', 'direct', 'component_lifecycle'].includes(controlManagementMode)) {
    return fail(
      `${path}.control_management_mode`,
      'pending, direct, or component_lifecycle',
    );
  }
  return {
    ownerEpoch: stringAt(identity.owner_epoch, `${path}.owner_epoch`),
    cellId: stringAt(identity.cell_id, `${path}.cell_id`),
    activePublicationGeneration: positiveIntegerAt(
      identity.active_publication_generation,
      `${path}.active_publication_generation`,
    ),
    controlManagementMode: controlManagementMode as ExecutionIdentity['controlManagementMode'],
    graphRevision: stringAt(identity.graph_revision, `${path}.graph_revision`),
    retainedBundleId: stringAt(
      identity.retained_bundle_id,
      `${path}.retained_bundle_id`,
    ),
    sceneRevision: sceneRevisionAt(
      identity.scene_revision,
      `${path}.scene_revision`,
    ),
  };
}

export function parseExecutionIdentity(value: unknown): ExecutionIdentity {
  const envelope = objectAt(value, 'executionIdentity');
  exactKeysAt(
    envelope,
    ['api_version', 'command', 'identity', 'ok'],
    'executionIdentity',
  );
  if (
    stringAt(envelope.api_version, 'executionIdentity.api_version')
      !== 'manyforge.execution_command_response.v1'
    || booleanAt(envelope.ok, 'executionIdentity.ok') !== true
    || stringAt(envelope.command, 'executionIdentity.command') !== 'identity'
  ) {
    return fail('executionIdentity', 'a successful identity response');
  }
  return executionIdentityAt(envelope.identity, 'executionIdentity.identity');
}

export function parseExecutionControlStatus(
  targetsValue: unknown,
  controlValue: unknown,
): ExecutionOwnerStatus {
  const targetsEnvelope = objectAt(targetsValue, 'executionTargets');
  exactKeysAt(
    targetsEnvelope,
    ['api_version', 'command', 'identity', 'ok', 'targets'],
    'executionTargets',
  );
  if (
    stringAt(targetsEnvelope.api_version, 'executionTargets.api_version')
      !== 'manyforge.execution_command_response.v1'
    || booleanAt(targetsEnvelope.ok, 'executionTargets.ok') !== true
    || stringAt(targetsEnvelope.command, 'executionTargets.command') !== 'targets'
  ) {
    return fail('executionTargets', 'a successful targets response');
  }
  const controlEnvelope = objectAt(controlValue, 'executionControlState');
  exactKeysAt(
    controlEnvelope,
    ['api_version', 'command', 'control_state', 'identity', 'ok'],
    'executionControlState',
  );
  if (
    stringAt(controlEnvelope.api_version, 'executionControlState.api_version')
      !== 'manyforge.execution_command_response.v1'
    || booleanAt(controlEnvelope.ok, 'executionControlState.ok') !== true
    || stringAt(controlEnvelope.command, 'executionControlState.command')
      !== 'control_state'
  ) {
    return fail('executionControlState', 'a successful control-state response');
  }
  const targetIdentity = executionIdentityAt(
    targetsEnvelope.identity,
    'executionTargets.identity',
  );
  const controlIdentity = executionIdentityAt(
    controlEnvelope.identity,
    'executionControlState.identity',
  );
  if (JSON.stringify(targetIdentity) !== JSON.stringify(controlIdentity)) {
    return fail('executionControlStatus', 'matching owner/product identities');
  }
  // execution_targets.v3 + execution_control_state.v6 (H1a-IF.2): every
  // managed cell uses one component-based shape; no primary target exists.
  const targets = objectAt(targetsEnvelope.targets, 'executionTargets.targets');
  allowedKeysAt(targets, ['components', 'schema_version'], ['io_signal_bindings'], 'executionTargets.targets');
  // Optional and static per activation: every target's bound boolean signals.
  const ioSignalBindings = targets.io_signal_bindings === undefined
    ? undefined
    : ioSignalBindingsAt(targets.io_signal_bindings, 'executionTargets.targets.io_signal_bindings');
  if (stringAt(targets.schema_version, 'executionTargets.targets.schema_version')
    !== 'manyforge.execution_targets.v3') {
    return fail('executionTargets.targets.schema_version', 'manyforge.execution_targets.v3');
  }
  const control = objectAt(
    controlEnvelope.control_state,
    'executionControlState.control_state',
  );
  exactKeysAt(control, [
    'cell_stop_action',
    'manual_operations',
    'observed_targets',
    'runtime_scale_control',
    'schema_version',
  ], 'executionControlState.control_state');
  if (stringAt(
    control.schema_version,
    'executionControlState.control_state.schema_version',
  ) !== 'manyforge.execution_control_state.v6') {
    return fail(
      'executionControlState.control_state.schema_version',
      'manyforge.execution_control_state.v6',
    );
  }
  const rowKey = (componentId: unknown, targetId: unknown, capabilityId: unknown) => (
    JSON.stringify([componentId, targetId, capabilityId])
  );
  const observedByKey = new Map<string, unknown>();
  boundedArrayAt(
    control.observed_targets,
    'executionControlState.control_state.observed_targets',
    512,
  ).forEach((entry, index) => {
    const path = `executionControlState.control_state.observed_targets[${index}]`;
    const row = objectAt(entry, path);
    exactKeysAt(row, [
      'command_target_id', 'controllable_component_id', 'observed', 'operation_capability_id',
    ], path);
    const key = rowKey(
      stringAt(row.controllable_component_id, `${path}.controllable_component_id`),
      stringAt(row.command_target_id, `${path}.command_target_id`),
      stringAt(row.operation_capability_id, `${path}.operation_capability_id`),
    );
    if (observedByKey.has(key)) {
      return fail(path, 'one observation row per commandable target');
    }
    observedByKey.set(key, row.observed);
  });
  // The v6 observation for one commandable target, in the internal shape
  // (without its kind discriminator), or null when not yet authoritative.
  const observationFor = (
    componentId: string,
    target: JsonObject,
    kind: 'joint_positions' | 'coordinate',
    path: string,
  ): unknown => {
    const key = rowKey(componentId, target.command_target_id, target.operation_capability_id);
    if (!observedByKey.has(key)) {
      return fail(path, 'one observation row for the exact addressed target');
    }
    const observed = observedByKey.get(key);
    observedByKey.delete(key);
    if (observed === null) return null;
    const record = objectAt(observed, `${path}.observed`);
    if (record.kind !== kind) return fail(`${path}.observed.kind`, kind);
    const rest = { ...record };
    delete rest.kind;
    return rest;
  };
  const normalizedComponents: JsonObject[] = [];
  const stopResponses = new Map<string, TargetStopResponse>();
  const motionRows: JsonObject[] = [];
  const gripperRows: JsonObject[] = [];
  boundedArrayAt(
    targets.components,
    'executionTargets.targets.components',
    128,
  ).forEach((entry, componentIndex) => {
    const componentPath = `executionTargets.targets.components[${componentIndex}]`;
    const component = objectAt(entry, componentPath);
    exactKeysAt(component, [
      'availability', 'controllable_component_id', 'targets',
    ], componentPath);
    const componentId = stringAt(
      component.controllable_component_id,
      `${componentPath}.controllable_component_id`,
    );
    const availability = objectAt(component.availability, `${componentPath}.availability`);
    exactKeysAt(availability, ['command_eligible', 'reason'], `${componentPath}.availability`);
    const unsupported: JsonObject[] = [];
    const targetKeys: string[] = [];
    boundedArrayAt(component.targets, `${componentPath}.targets`, 128)
      .forEach((targetEntry, targetIndex) => {
        const targetPath = `${componentPath}.targets[${targetIndex}]`;
        const target = objectAt(targetEntry, targetPath);
        exactKeysAt(target, [
          'command', 'command_target_id', 'operation_capability_id', 'presentation',
        ], targetPath);
        const commandTargetId = stringAt(target.command_target_id, `${targetPath}.command_target_id`);
        const operationCapabilityId = stringAt(
          target.operation_capability_id,
          `${targetPath}.operation_capability_id`,
        );
        targetKeys.push(JSON.stringify([commandTargetId, operationCapabilityId]));
        const presentation = stringAt(target.presentation, `${targetPath}.presentation`);
        if (presentation === 'unsupported') {
          if (target.command !== null) return fail(`${targetPath}.command`, 'null');
          unsupported.push({
            command_target_id: commandTargetId,
            operation_capability_id: operationCapabilityId,
          });
          return;
        }
        const command = objectAt(target.command, `${targetPath}.command`);
        const manual = objectAt(command.manual_control, `${targetPath}.command.manual_control`);
        exactKeysAt(manual, [
          'program_binding_id', 'program_id', 'program_revision', 'program_run_id',
        ], `${targetPath}.command.manual_control`);
        const commandFields = { ...command };
        delete commandFields.manual_control;
        const addressed = {
          ...commandFields,
          ...manual,
          controllable_component_id: componentId,
          command_target_id: commandTargetId,
          operation_capability_id: operationCapabilityId,
        };
        if (presentation === 'motion') {
          allowedKeysAt(command, [
            'base_frame_role_id', 'component_role_id', 'frame_role_ids', 'joints',
            'manual_control', 'planning_capability_ids', 'target_descriptor',
            'target_role_id', 'tcp_frame_role_id',
          ], ['stop_response', 'stop_response_error'], `${targetPath}.command`);
          // The execution host publishes `stop_response: null` with the
          // owner's error when it cannot read the target's Stop metadata.
          // That is a fact about this target, never a malformed snapshot.
          if (command.stop_response === null) {
            const error = objectAt(command.stop_response_error, `${targetPath}.command.stop_response_error`);
            allowedKeysAt(error, ['code', 'message'], ['details'], `${targetPath}.command.stop_response_error`);
            stringAt(error.code, `${targetPath}.command.stop_response_error.code`);
            stringAt(error.message, `${targetPath}.command.stop_response_error.message`);
          } else if (command.stop_response_error !== undefined) {
            return fail(`${targetPath}.command.stop_response_error`, 'present only without a Stop response');
          } else if (command.stop_response !== undefined) {
            stopResponses.set(
              JSON.stringify([componentId, commandTargetId]),
              targetStopResponseAt(command.stop_response, `${targetPath}.command.stop_response`),
            );
          }
          delete (addressed as JsonObject).stop_response;
          delete (addressed as JsonObject).stop_response_error;
          motionRows.push({
            ...addressed,
            observed_state: observationFor(componentId, target, 'joint_positions', targetPath),
          });
        } else if (presentation === 'gripper') {
          exactKeysAt(command, [
            'command_schema_id', 'command_schema_revision', 'component_role_id',
            'manual_control', 'resource', 'target_descriptor', 'target_role_id',
          ], `${targetPath}.command`);
          gripperRows.push({
            ...addressed,
            observed_state: observationFor(componentId, target, 'coordinate', targetPath),
          });
        } else {
          return fail(`${targetPath}.presentation`, 'motion, gripper, or unsupported');
        }
      });
    if (targetKeys.some((key, index) => index > 0 && targetKeys[index - 1] >= key)) {
      return fail(`${componentPath}.targets`, 'unique targets sorted by target and capability');
    }
    normalizedComponents.push({
      controllable_component_id: componentId,
      command_eligible: availability.command_eligible,
      reason: availability.reason,
      unsupported_targets: unsupported,
    });
  });
  if (observedByKey.size !== 0) {
    return fail(
      'executionControlState.control_state.observed_targets',
      'observations only for commandable addressed targets',
    );
  }
  const identity = targetIdentity;
  const allowedProgramRuns = new Set([...motionRows, ...gripperRows].map((row, index) => stringAt(
    row.program_run_id,
    `executionTargets.targets.program_run_id[${index}]`,
  )));
  const manualRows = arrayAt(
    control.manual_operations,
    'executionControlState.control_state.manual_operations',
  );
  if (manualRows.length > 128) {
    return fail('executionControlState.control_state.manual_operations', 'a bounded operation list');
  }
  const manualOperations = manualRows.map((value, index) => {
    const projection = objectAt(
      value,
      `executionControlState.control_state.manual_operations[${index}]`,
    );
    exactKeysAt(projection, ['operation', 'resident'],
      `executionControlState.control_state.manual_operations[${index}]`);
    const operation = objectAt(projection.operation,
      `executionControlState.control_state.manual_operations[${index}].operation`);
    const programRunId = stringAt(
      operation.program_run_id,
      `executionControlState.control_state.manual_operations[${index}].program_run_id`,
    );
    if (!allowedProgramRuns.has(programRunId)) {
      return fail(
        `executionControlState.control_state.manual_operations[${index}].program_run_id`,
        'one of the exact direct-control Program runs',
      );
    }
    const observed = parseOperationObservation({
      command: 'read',
      ok: true,
      operation,
      resident: projection.resident,
    });
    if (!observed.resident && ![
      'completed', 'stopped', 'failed', 'interrupted', 'intervention_required', 'cancelled_pre_dispatch',
    ].includes(observed.state)) {
      return fail('executionControlState.control_state.manual_operations',
        'a terminal state for nonresident results');
    }
    return { ...observed, programRunId };
  });
  if (new Set(manualOperations.map((operation) => operation.operationId)).size
      !== manualOperations.length
    || new Set(manualOperations.map((operation) => operation.programRunId)).size
      !== manualOperations.length) {
    return fail(
      'executionControlState.control_state.manual_operations',
      'unique operation and manual-target identities',
    );
  }
  const status = normalizedOwnerStatusAt({
    api_version: 'manyforge.execution_command_response.v1',
    command: 'status',
    ok: true,
    status: {
      active_publication_generation: identity.activePublicationGeneration,
      cell_id: identity.cellId,
      cell_stop_action: control.cell_stop_action,
      components: normalizedComponents,
      // The first addressed targets are only the initial UI selection.
      direct_control: motionRows[0] ?? null,
      direct_control_gripper: gripperRows[0] ?? null,
      direct_control_targets: motionRows,
      direct_control_gripper_targets: gripperRows,
      graph_revision: identity.graphRevision,
      owner_epoch: identity.ownerEpoch,
      retained_bundle_id: identity.retainedBundleId,
      scene_revision: identity.sceneRevision,
    },
  });
  // Null when no motion target supports scaling (for example gripper-only).
  const runtimeScaleControl = control.runtime_scale_control === null
    ? null
    : runtimeScaleControlStatusAt(
      control.runtime_scale_control,
      'executionControlState.control_state.runtime_scale_control',
    );
  if (runtimeScaleControl !== null && runtimeScaleControl.selector.kind === 'singular') {
    const selector = runtimeScaleControl.selector;
    if (!status.directControlTargets.some((target) => (
      selector.programBindingId === target.programBindingId
      && selector.componentRoleId === target.componentRoleId
      && selector.targetRoleId === target.targetRoleId
    ))) {
      return fail(
        'executionControlState.control_state.runtime_scale_control.selector',
        'an addressed direct-control motion target',
      );
    }
  }
  const withStopResponse = (target: DirectControlMotionStatus): DirectControlMotionStatus => {
    const stopResponse = stopResponses.get(
      JSON.stringify([target.controllableComponentId, target.commandTargetId]),
    );
    return stopResponse === undefined ? target : { ...target, stopResponse };
  };
  return {
    ...status,
    directControl: status.directControl === null ? null : withStopResponse(status.directControl),
    directControlTargets: status.directControlTargets.map(withStopResponse),
    manualOperations,
    runtimeScaleControl,
    ...(ioSignalBindings === undefined ? {} : { ioSignalBindings }),
  };
}

/**
 * The optional `target.command.stop_response` (SX-2167): strict, with its
 * stages in qualified chain order and unique stage ids.
 */
export function targetStopResponseAt(value: unknown, path: string): TargetStopResponse {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'control_binding_id', 'control_binding_revision', 'control_route_id', 'nominal_stop_mode', 'stages',
  ], path);
  const text = (field: string) => {
    const entry = stringAt(record[field], `${path}.${field}`);
    return entry === '' ? fail(`${path}.${field}`, 'nonempty text') : entry;
  };
  const stages = boundedArrayAt(record.stages, `${path}.stages`, 16).map((item, index) => {
    const stagePath = `${path}.stages[${index}]`;
    const stage = objectAt(item, stagePath);
    exactKeysAt(stage, ['command_kind', 'stage_id', 'stage_kind'], stagePath);
    const field = (key: string) => {
      const entry = stringAt(stage[key], `${stagePath}.${key}`);
      return entry === '' ? fail(`${stagePath}.${key}`, 'nonempty text') : entry;
    };
    return { stageId: field('stage_id'), stageKind: field('stage_kind'), commandKind: field('command_kind') };
  });
  if (stages.length === 0) return fail(`${path}.stages`, 'at least one qualified stage');
  if (new Set(stages.map((stage) => stage.stageId)).size !== stages.length) {
    return fail(`${path}.stages`, 'unique stage ids');
  }
  return {
    controlBindingId: text('control_binding_id'),
    controlBindingRevision: text('control_binding_revision'),
    controlRouteId: text('control_route_id'),
    nominalStopMode: nullableStringAt(record.nominal_stop_mode, `${path}.nominal_stop_mode`),
    stages,
  };
}

/**
 * One addressed direct-control motion target and its observation (internal
 * normalized shape; the wire contract is execution_targets.v3/control v6).
 */
function directControlMotionAt(value: unknown): {
  directControl: DirectControlMotionStatus;
  observedMotion: ObservedMotionTarget | null;
} {
  const status: JsonObject = { direct_control: value };
  const direct = objectAt(
    status.direct_control,
    'executionStatus.status.direct_control',
  );
  exactKeysAt(direct, [
    'base_frame_role_id',
    'command_target_id',
    'component_role_id',
    'controllable_component_id',
    'frame_role_ids',
    'joints',
    'observed_state',
    'operation_capability_id',
    'planning_capability_ids',
    'program_binding_id',
    'program_id',
    'program_revision',
    'program_run_id',
    'target_role_id',
    'target_descriptor',
    'tcp_frame_role_id',
  ], 'executionStatus.status.direct_control');
  const motionDescriptor = manualControlMotionDescriptorAt(
    direct.target_descriptor,
    'executionStatus.status.direct_control.target_descriptor',
  );
  const joints = arrayAt(
    direct.joints,
    'executionStatus.status.direct_control.joints',
  ).map((item, index) => directControlJointAt(
    item,
    `executionStatus.status.direct_control.joints[${index}]`,
  ));
  if (joints.length === 0) {
    return fail('executionStatus.status.direct_control.joints', 'non-empty');
  }
  const sourceNames = joints.map((joint) => joint.sourceJointName);
  if (new Set(sourceNames).size !== sourceNames.length) {
    return fail(
      'executionStatus.status.direct_control.joints',
      'unique source joint names',
    );
  }
  for (const [index, joint] of joints.entries()) {
    if (
      joint.minPositionRad > joint.maxPositionRad
      || joint.maxVelocityRadS <= 0
      || (joint.maxAccelerationRadS2 !== null
        && joint.maxAccelerationRadS2 <= 0)
    ) {
      return fail(
        `executionStatus.status.direct_control.joints[${index}]`,
        'ordered finite limits',
      );
    }
  }
  const componentId = stringAt(
    direct.controllable_component_id,
    'executionStatus.status.direct_control.controllable_component_id',
  );
  const targetId = stringAt(
    direct.command_target_id,
    'executionStatus.status.direct_control.command_target_id',
  );
  // Null until the owner has an authoritative observation (IF.2).
  let observedMotion: ObservedMotionTarget | null = null;
  if (direct.observed_state !== null) {
    const observed = objectAt(
      direct.observed_state,
      'executionStatus.status.direct_control.observed_state',
    );
    exactKeysAt(observed, [
      'complete',
      'current_tcp_pose',
      'positions_rad',
      'sequence',
      'source_epoch',
      'source_joint_names',
    ], 'executionStatus.status.direct_control.observed_state');
    const observedNames = stringArrayAt(
      observed.source_joint_names,
      'executionStatus.observedMotion.joint_names',
    );
    const positions = arrayAt(
      observed.positions_rad,
      'executionStatus.observedMotion.positions_rad',
    ).map((item, index) => numberAt(
      item,
      `executionStatus.observedMotion.positions_rad[${index}]`,
    ));
    if (
      observedNames.length !== sourceNames.length
      || observedNames.some((name, index) => name !== sourceNames[index])
      || positions.length !== sourceNames.length
    ) {
      return fail(
        'executionStatus.observedMotion',
        'the exact source-local direct-control joint order',
      );
    }
    observedMotion = {
      targetRoleId: stringAt(
        direct.target_role_id,
        'executionStatus.status.direct_control.target_role_id',
      ),
      controllableComponentId: componentId,
      commandTargetId: targetId,
      sequence: integerAt(
        observed.sequence,
        'executionStatus.observedMotion.sequence',
      ),
      sourceEpoch: stringAt(
        observed.source_epoch,
        'executionStatus.observedMotion.source_epoch',
      ),
      sourceJointNames: observedNames,
      positionsRad: positions,
      complete: booleanAt(
        observed.complete,
        'executionStatus.observedMotion.complete',
      ),
      currentTcpPose: poseAt(
        observed.current_tcp_pose,
        'executionStatus.observedMotion.current_tcp_pose',
      ),
    };
  }
  const frameRoleIds = stringArrayAt(
    direct.frame_role_ids,
    'executionStatus.status.direct_control.frame_role_ids',
  );
  const planningCapabilityIds = stringArrayAt(
    direct.planning_capability_ids,
    'executionStatus.status.direct_control.planning_capability_ids',
  );
  const descriptorFrameRoleIds = motionDescriptor.frames.map(
    (frame) => frame.frameRoleId,
  );
  const descriptorSupportingCapabilityIds = motionDescriptor.contracts
    .supportingProviders.map((provider) => provider.capabilityId);
  if (
    motionDescriptor.controllableComponentId !== componentId
    || motionDescriptor.commandTargetId !== targetId
    || motionDescriptor.componentRoleId !== direct.component_role_id
    || motionDescriptor.targetRoleId !== direct.target_role_id
    || motionDescriptor.contracts.operationCapabilityId !== direct.operation_capability_id
    || frameRoleIds.length !== descriptorFrameRoleIds.length
    || frameRoleIds.some((role, index) => role !== descriptorFrameRoleIds[index])
    || planningCapabilityIds.length !== descriptorSupportingCapabilityIds.length
    || planningCapabilityIds.some(
      (capability, index) => capability !== descriptorSupportingCapabilityIds[index],
    )
    || motionDescriptor.joints.length !== joints.length
    || motionDescriptor.joints.some((joint, index) => {
      const statusJoint = joints[index];
      return statusJoint === undefined
        || joint.sourceJointName !== statusJoint.sourceJointName
        || joint.minPositionRad !== statusJoint.minPositionRad
        || joint.maxPositionRad !== statusJoint.maxPositionRad
        || joint.maxVelocityRadS !== statusJoint.maxVelocityRadS
        || joint.maxAccelerationRadS2 !== statusJoint.maxAccelerationRadS2;
    })
  ) {
    return fail('executionStatus.status.direct_control', 'descriptors joined to the exact addressed targets');
  }
  return {
    directControl: {
      controllableComponentId: componentId,
      commandTargetId: targetId,
      componentRoleId: stringAt(
        direct.component_role_id,
        'executionStatus.status.direct_control.component_role_id',
      ),
      targetRoleId: stringAt(
        direct.target_role_id,
        'executionStatus.status.direct_control.target_role_id',
      ),
      baseFrameRoleId: stringAt(
        direct.base_frame_role_id,
        'executionStatus.status.direct_control.base_frame_role_id',
      ),
      tcpFrameRoleId: stringAt(
        direct.tcp_frame_role_id,
        'executionStatus.status.direct_control.tcp_frame_role_id',
      ),
      operationCapabilityId: stringAt(
        direct.operation_capability_id,
        'executionStatus.status.direct_control.operation_capability_id',
      ),
      planningCapabilityIds,
      frameRoleIds,
      programId: stringAt(
        direct.program_id,
        'executionStatus.status.direct_control.program_id',
      ),
      programRevision: stringAt(
        direct.program_revision,
        'executionStatus.status.direct_control.program_revision',
      ),
      programBindingId: stringAt(
        direct.program_binding_id,
        'executionStatus.status.direct_control.program_binding_id',
      ),
      programRunId: stringAt(
        direct.program_run_id,
        'executionStatus.status.direct_control.program_run_id',
      ),
      joints,
      targetDescriptor: motionDescriptor,
        },
    observedMotion,
  };
}

/**
 * The addressed direct-control gripper, or null for an arm-only component
 * that has no gripper target at all (never a placeholder).
 */
function directControlGripperAt(value: unknown): {
  directControlGripper: DirectControlGripperStatus | null;
  observedGripper: ObservedGripperTarget | null;
} {
  if (value === null) return { directControlGripper: null, observedGripper: null };
  const status: JsonObject = { direct_control_gripper: value };
  const directGripper = objectAt(
    status.direct_control_gripper,
    'executionStatus.status.direct_control_gripper',
  );
  exactKeysAt(directGripper, [
    'command_schema_id',
    'command_schema_revision',
    'command_target_id',
    'component_role_id',
    'controllable_component_id',
    'observed_state',
    'operation_capability_id',
    'program_binding_id',
    'program_id',
    'program_revision',
    'program_run_id',
    'resource',
    'target_role_id',
    'target_descriptor',
  ], 'executionStatus.status.direct_control_gripper');
  const gripperDescriptor = manualControlGripperDescriptorAt(
    directGripper.target_descriptor,
    'executionStatus.status.direct_control_gripper.target_descriptor',
  );
  const gripperResource = objectAt(
    directGripper.resource,
    'executionStatus.status.direct_control_gripper.resource',
  );
  exactKeysAt(gripperResource, [
    'coordinate_kind',
    'max_position',
    'min_position',
    'source_coordinate_name',
    'source_kind',
  ], 'executionStatus.status.direct_control_gripper.resource');
  const coordinateKind = stringAt(
    gripperResource.coordinate_kind,
    'executionStatus.status.direct_control_gripper.resource.coordinate_kind',
  );
  if (coordinateKind !== 'angular_rad' && coordinateKind !== 'linear_m') {
    return fail(
      'executionStatus.status.direct_control_gripper.resource.coordinate_kind',
      'angular_rad or linear_m',
    );
  }
  const sourceKind = stringAt(
    gripperResource.source_kind,
    'executionStatus.status.direct_control_gripper.resource.source_kind',
  );
  if (sourceKind !== 'model_role' && sourceKind !== 'component_local') {
    return fail(
      'executionStatus.status.direct_control_gripper.resource.source_kind',
      'model_role or component_local',
    );
  }
  const gripperMin = nullableNumberAt(
    gripperResource.min_position,
    'executionStatus.status.direct_control_gripper.resource.min_position',
  );
  const gripperMax = nullableNumberAt(
    gripperResource.max_position,
    'executionStatus.status.direct_control_gripper.resource.max_position',
  );
  if (
    (sourceKind === 'model_role'
      && (gripperMin === null
        || gripperMax === null
        || gripperMin > gripperMax))
    || (sourceKind === 'component_local'
      && (gripperMin !== null || gripperMax !== null))
  ) {
    return fail(
      'executionStatus.status.direct_control_gripper.resource',
      sourceKind === 'model_role'
        ? 'ordered finite model limits'
        : 'no fabricated model limits for a component-local coordinate',
    );
  }
  const gripperSourceName = stringAt(
    gripperResource.source_coordinate_name,
    'executionStatus.status.direct_control_gripper.resource.source_coordinate_name',
  );
  if (directGripper.observed_state === null && sourceKind !== 'component_local'
    && gripperDescriptor.actuationKind !== 'pneumatic') {
    return fail(
      'executionStatus.status.direct_control_gripper.observed_state',
      'a model-backed observation',
    );
  }
  let observedGripper: ObservedGripperTarget | null = null;
  if (directGripper.observed_state !== null) {
    const gripperObserved = objectAt(
      directGripper.observed_state,
      'executionStatus.status.direct_control_gripper.observed_state',
    );
    exactKeysAt(gripperObserved, [
      'complete',
      'position',
      'sequence',
      'source_coordinate_name',
      'source_epoch',
    ], 'executionStatus.status.direct_control_gripper.observed_state');
    const observedPosition = objectAt(
      gripperObserved.position,
      'executionStatus.status.direct_control_gripper.observed_state.position',
    );
    const expectedPositionKey = coordinateKind === 'angular_rad'
      ? 'value_rad'
      : 'value_m';
    exactKeysAt(
      observedPosition,
      ['kind', expectedPositionKey],
      'executionStatus.status.direct_control_gripper.observed_state.position',
    );
    if (stringAt(
      observedPosition.kind,
      'executionStatus.status.direct_control_gripper.observed_state.position.kind',
    ) !== coordinateKind) {
      return fail(
        'executionStatus.status.direct_control_gripper.observed_state.position.kind',
        coordinateKind,
      );
    }
    if (stringAt(
      gripperObserved.source_coordinate_name,
      'executionStatus.status.direct_control_gripper.observed_state.source_coordinate_name',
    ) !== gripperSourceName) {
      return fail(
        'executionStatus.status.direct_control_gripper.observed_state.source_coordinate_name',
        'the exact retained source coordinate',
      );
    }
    observedGripper = {
      targetRoleId: stringAt(
        directGripper.target_role_id,
        'executionStatus.status.direct_control_gripper.target_role_id',
      ),
      controllableComponentId: stringAt(
        directGripper.controllable_component_id,
        'executionStatus.status.direct_control_gripper.controllable_component_id',
      ),
      commandTargetId: stringAt(
        directGripper.command_target_id,
        'executionStatus.status.direct_control_gripper.command_target_id',
      ),
      sequence: integerAt(
        gripperObserved.sequence,
        'executionStatus.status.direct_control_gripper.observed_state.sequence',
      ),
      sourceEpoch: stringAt(
        gripperObserved.source_epoch,
        'executionStatus.status.direct_control_gripper.observed_state.source_epoch',
      ),
      sourceCoordinateName: gripperSourceName,
      position: numberAt(
        observedPosition[expectedPositionKey],
        `executionStatus.status.direct_control_gripper.observed_state.position.${expectedPositionKey}`,
      ),
      coordinateKind: coordinateKind as CoordinateKind,
      complete: booleanAt(
        gripperObserved.complete,
        'executionStatus.status.direct_control_gripper.observed_state.complete',
      ),
    };
  }
  if (
    gripperDescriptor.controllableComponentId !== directGripper.controllable_component_id
    || gripperDescriptor.commandTargetId !== directGripper.command_target_id
    || gripperDescriptor.componentRoleId !== directGripper.component_role_id
    || gripperDescriptor.targetRoleId !== directGripper.target_role_id
    || gripperDescriptor.sourceCoordinateName !== gripperSourceName
    || gripperDescriptor.sourceKind !== sourceKind
    || gripperDescriptor.coordinateKind !== coordinateKind
    || gripperDescriptor.minPosition !== gripperMin
    || gripperDescriptor.maxPosition !== gripperMax
    || gripperDescriptor.contracts.operationCapabilityId !== directGripper.operation_capability_id
    || gripperDescriptor.contracts.commandSchema?.commandSchemaId !== directGripper.command_schema_id
    || gripperDescriptor.contracts.commandSchema?.commandSchemaRevision !== directGripper.command_schema_revision
  ) {
    return fail(
      'executionStatus.status.direct_control_gripper',
      'descriptors joined to the exact addressed gripper target',
    );
  }
  return {
    directControlGripper: {
      controllableComponentId: stringAt(
        directGripper.controllable_component_id,
        'executionStatus.status.direct_control_gripper.controllable_component_id',
      ),
      commandTargetId: stringAt(
        directGripper.command_target_id,
        'executionStatus.status.direct_control_gripper.command_target_id',
      ),
      componentRoleId: stringAt(
        directGripper.component_role_id,
        'executionStatus.status.direct_control_gripper.component_role_id',
      ),
      targetRoleId: stringAt(
        directGripper.target_role_id,
        'executionStatus.status.direct_control_gripper.target_role_id',
      ),
      operationCapabilityId: stringAt(
        directGripper.operation_capability_id,
        'executionStatus.status.direct_control_gripper.operation_capability_id',
      ),
      programId: stringAt(
        directGripper.program_id,
        'executionStatus.status.direct_control_gripper.program_id',
      ),
      programRevision: stringAt(
        directGripper.program_revision,
        'executionStatus.status.direct_control_gripper.program_revision',
      ),
      programBindingId: stringAt(
        directGripper.program_binding_id,
        'executionStatus.status.direct_control_gripper.program_binding_id',
      ),
      programRunId: stringAt(
        directGripper.program_run_id,
        'executionStatus.status.direct_control_gripper.program_run_id',
      ),
      commandSchemaId: stringAt(
        directGripper.command_schema_id,
        'executionStatus.status.direct_control_gripper.command_schema_id',
      ),
      commandSchemaRevision: stringAt(
        directGripper.command_schema_revision,
        'executionStatus.status.direct_control_gripper.command_schema_revision',
      ),
      sourceCoordinateName: gripperSourceName,
      sourceKind: sourceKind as ResourceSourceKind,
      coordinateKind: coordinateKind as CoordinateKind,
      minPosition: gripperMin,
      maxPosition: gripperMax,
      targetDescriptor: gripperDescriptor,
        },
    observedGripper,
  };
}

/**
 * Aggregate status/diagnostics (`manyforge.execution_status.v2`, H1a-IF.2):
 * a pure composition of one owner snapshot's identity.v2, targets.v3 and
 * control_state.v6 payloads, normalized exactly like the separate commands.
 */
export function parseExecutionOwnerStatus(value: unknown): ExecutionOwnerStatus {
  const envelope = objectAt(value, 'executionStatus');
  exactKeysAt(envelope, ['api_version', 'command', 'ok', 'status'], 'executionStatus');
  if (
    stringAt(envelope.api_version, 'executionStatus.api_version')
      !== 'manyforge.execution_command_response.v1'
    || booleanAt(envelope.ok, 'executionStatus.ok') !== true
    || stringAt(envelope.command, 'executionStatus.command') !== 'status'
  ) {
    return fail('executionStatus', 'a successful status response');
  }
  const status = objectAt(envelope.status, 'executionStatus.status');
  exactKeysAt(status, [
    'control_state', 'identity', 'schema_version', 'targets',
  ], 'executionStatus.status');
  if (stringAt(status.schema_version, 'executionStatus.status.schema_version')
    !== 'manyforge.execution_status.v2') {
    return fail('executionStatus.status.schema_version', 'manyforge.execution_status.v2');
  }
  return parseExecutionControlStatus({
    api_version: 'manyforge.execution_command_response.v1',
    command: 'targets',
    identity: status.identity,
    ok: true,
    targets: status.targets,
  }, {
    api_version: 'manyforge.execution_command_response.v1',
    command: 'control_state',
    control_state: status.control_state,
    identity: status.identity,
    ok: true,
  });
}

/** Internal normalized owner status; not a wire contract. */
function normalizedOwnerStatusAt(value: unknown): ExecutionOwnerStatus {
  const envelope = objectAt(value, 'executionStatus');
  if (
    booleanAt(envelope.ok, 'executionStatus.ok') !== true
    || stringAt(envelope.command, 'executionStatus.command') !== 'status'
  ) {
    return fail('executionStatus', 'a successful status response');
  }
  const status = objectAt(envelope.status, 'executionStatus.status');
  // Null exactly for a component without motion capability (H1a-IF.2).
  const motion = status.direct_control === null
    ? null
    : directControlMotionAt(status.direct_control);
  // Null exactly for an arm-only component (no gripper target).
  const { directControlGripper, observedGripper } = directControlGripperAt(
    status.direct_control_gripper,
  );
  const certificateRefAt = (input: unknown, path: string) => {
    const record = objectAt(input, path);
    exactKeysAt(record, [
      'stop_certificate_id', 'stop_certificate_revision',
      'stop_generation', 'stop_id',
    ], path);
    return {
      stopCertificateId: stringAt(
        record.stop_certificate_id,
        `${path}.stop_certificate_id`,
      ),
      stopCertificateRevision: positiveIntegerAt(
        record.stop_certificate_revision,
        `${path}.stop_certificate_revision`,
      ),
      stopId: stringAt(record.stop_id, `${path}.stop_id`),
      stopGeneration: positiveIntegerAt(
        record.stop_generation,
        `${path}.stop_generation`,
      ),
    };
  };
  const cellStop = objectAt(
    status.cell_stop_action,
    'executionStatus.status.cell_stop_action',
  );
  exactKeysAt(cellStop, [
    'available', 'expected_stop_generation', 'fault', 'latest_terminal',
    'unavailable_reason',
  ], 'executionStatus.status.cell_stop_action');
  const cellStopAvailable = booleanAt(
    cellStop.available,
    'executionStatus.status.cell_stop_action.available',
  );
  const cellStopUnavailableReason = nullableStringAt(
    cellStop.unavailable_reason,
    'executionStatus.status.cell_stop_action.unavailable_reason',
  );
  if (cellStopAvailable === (cellStopUnavailableReason !== null)) {
    return fail(
      'executionStatus.status.cell_stop_action',
      'an unavailable reason exactly when the action is unavailable',
    );
  }
  let latestTerminal: ExecutionOwnerStatus['cellStopAction']['latestTerminal'] = null;
  if (cellStop.latest_terminal !== null) {
    const terminal = objectAt(
      cellStop.latest_terminal,
      'executionStatus.status.cell_stop_action.latest_terminal',
    );
    exactKeysAt(terminal, [
      'certificate', 'escalated', 'executed_stages',
      'hard_stop_floor_used', 'result',
    ], 'executionStatus.status.cell_stop_action.latest_terminal');
    const result = stringAt(
      terminal.result,
      'executionStatus.status.cell_stop_action.latest_terminal.result',
    );
    if (![
      'succeeded_nominal', 'succeeded_escalated', 'partial',
      'failed_uncontained',
    ].includes(result)) {
      return fail(
        'executionStatus.status.cell_stop_action.latest_terminal.result',
        'a Stop certificate result',
      );
    }
    const executedStages = arrayAt(
      terminal.executed_stages,
      'executionStatus.status.cell_stop_action.latest_terminal.executed_stages',
    ).map((item, index) => {
      const path = `executionStatus.status.cell_stop_action.latest_terminal.executed_stages[${index}]`;
      const stage = objectAt(item, path);
      exactKeysAt(stage, [
        'action_class', 'executed_mode', 'operation_id', 'outcome',
        'participant_id', 'stage_id', 'stage_index', 'substitution_reason',
      ], path);
      const actionClass = stringAt(stage.action_class, `${path}.action_class`);
      if (!['controlled', 'software_hard', 'safety_system_request'].includes(actionClass)) {
        return fail(`${path}.action_class`, 'a Stop action class');
      }
      const outcome = stringAt(stage.outcome, `${path}.outcome`);
      if (!['succeeded', 'partial', 'failed', 'superseded_no_effect'].includes(outcome)) {
        return fail(`${path}.outcome`, 'a Stop stage outcome');
      }
      const executedMode = nullableStringAt(
        stage.executed_mode,
        `${path}.executed_mode`,
      );
      if (executedMode !== null && ![
        'soft_on_path', 'spring_back_hold', 'spring_back_fast', 'hard_deceleration',
      ].includes(executedMode)) {
        return fail(`${path}.executed_mode`, 'a controlled Stop mode');
      }
      return {
        operationId: stringAt(stage.operation_id, `${path}.operation_id`),
        participantId: stringAt(stage.participant_id, `${path}.participant_id`),
        stageId: stringAt(stage.stage_id, `${path}.stage_id`),
        stageIndex: integerAt(stage.stage_index, `${path}.stage_index`),
        actionClass: actionClass as 'controlled' | 'software_hard' | 'safety_system_request',
        outcome: outcome as 'succeeded' | 'partial' | 'failed' | 'superseded_no_effect',
        executedMode: executedMode as 'soft_on_path' | 'spring_back_hold' | 'spring_back_fast' | 'hard_deceleration' | null,
        substitutionReason: nullableStringAt(
          stage.substitution_reason,
          `${path}.substitution_reason`,
        ),
      };
    });
    latestTerminal = {
      certificate: certificateRefAt(
        terminal.certificate,
        'executionStatus.status.cell_stop_action.latest_terminal.certificate',
      ),
      result: result as 'succeeded_nominal' | 'succeeded_escalated' | 'partial' | 'failed_uncontained',
      escalated: booleanAt(
        terminal.escalated,
        'executionStatus.status.cell_stop_action.latest_terminal.escalated',
      ),
      hardStopFloorUsed: booleanAt(
        terminal.hard_stop_floor_used,
        'executionStatus.status.cell_stop_action.latest_terminal.hard_stop_floor_used',
      ),
      executedStages,
    };
  }
  let cellStopFault: ExecutionOwnerStatus['cellStopAction']['fault'] = null;
  if (cellStop.fault !== null) {
    const path = 'executionStatus.status.cell_stop_action.fault';
    const fault = objectAt(cellStop.fault, path);
    exactKeysAt(fault, [
      'certificate', 'latch_generation', 'latch_id', 'reason_code',
      'recovery_status', 'state', 'trigger',
    ], path);
    const stateName = stringAt(fault.state, `${path}.state`);
    if (!['latched', 'recovery_pending', 'cleared'].includes(stateName)) {
      return fail(`${path}.state`, 'a Stop fault latch state');
    }
    const trigger = stringAt(fault.trigger, `${path}.trigger`);
    if (![
      'scope_promotion', 'escalated', 'partial', 'failed_uncontained',
      'physical_rest_unproven', 'persistence_degraded',
    ].includes(trigger)) {
      return fail(`${path}.trigger`, 'a Stop fault trigger');
    }
    const recoveryStatus = stringAt(
      fault.recovery_status,
      `${path}.recovery_status`,
    );
    if (![
      'waiting_for_observation', 'waiting_for_quiescence',
      'waiting_for_configuration', 'waiting_for_external_reconciliation',
      'waiting_for_durable_record', 'ready', 'recovery_pending', 'cleared',
    ].includes(recoveryStatus)) {
      return fail(`${path}.recovery_status`, 'a Stop fault recovery status');
    }
    cellStopFault = {
      latchId: stringAt(fault.latch_id, `${path}.latch_id`),
      latchGeneration: positiveIntegerAt(
        fault.latch_generation,
        `${path}.latch_generation`,
      ),
      state: stateName as 'latched' | 'recovery_pending' | 'cleared',
      trigger: trigger as NonNullable<ExecutionOwnerStatus['cellStopAction']['fault']>['trigger'],
      recoveryStatus: recoveryStatus as NonNullable<ExecutionOwnerStatus['cellStopAction']['fault']>['recoveryStatus'],
      reasonCode: stringAt(fault.reason_code, `${path}.reason_code`),
      certificate: fault.certificate === null
        ? null
        : certificateRefAt(fault.certificate, `${path}.certificate`),
    };
  }
  // Every addressed target is parsed on its own: a gripper never needs a
  // motion target on the same component, and neither needs the other.
  const motionEntries = arrayAt(
    status.direct_control_targets,
    'executionStatus.status.direct_control_targets',
  ).map((entry) => directControlMotionAt(entry));
  const gripperEntries = arrayAt(
    status.direct_control_gripper_targets,
    'executionStatus.status.direct_control_gripper_targets',
  ).map((entry) => directControlGripperAt(entry));
  const directControlTargets = motionEntries.map((entry) => entry.directControl);
  const directControlGripperTargets = gripperEntries
    .map((entry) => entry.directControlGripper)
    .filter((entry): entry is DirectControlGripperStatus => entry !== null);
  const targetKey = (target: {
    controllableComponentId: string;
    commandTargetId: string;
    operationCapabilityId: string;
  }) => `${target.controllableComponentId}\u0000${target.commandTargetId}\u0000${target.operationCapabilityId}`;
  const targetKeys = [
    ...directControlTargets.map(targetKey),
    ...directControlGripperTargets.map(targetKey),
  ];
  if (new Set(targetKeys).size !== targetKeys.length) {
    return fail(
      'executionStatus.status.direct_control_targets',
      'unique component/target/capability identities',
    );
  }
  // A selected alias must be one of the addressed targets, never extra.
  if ((motion !== null && !targetKeys.includes(targetKey(motion.directControl)))
    || (directControlGripper !== null
      && !targetKeys.includes(targetKey(directControlGripper)))) {
    return fail(
      'executionStatus.status.direct_control',
      'a selected target that is one of the addressed targets',
    );
  }
  const components = executionComponentsAt(
    status.components,
    'executionStatus.status.components',
  );
  const componentIds = new Set(components.map((component) => component.controllableComponentId));
  if ([...directControlTargets, ...directControlGripperTargets]
    .some((target) => !componentIds.has(target.controllableComponentId))) {
    return fail(
      'executionStatus.status.components',
      'every addressed target owned by a listed component',
    );
  }
  const observedGrippers = gripperEntries
    .map((entry) => entry.observedGripper)
    .filter((entry): entry is ObservedGripperTarget => entry !== null);
  return {
    ownerEpoch: stringAt(status.owner_epoch, 'executionStatus.status.owner_epoch'),
    cellId: stringAt(status.cell_id, 'executionStatus.status.cell_id'),
    activePublicationGeneration: positiveIntegerAt(
      status.active_publication_generation,
      'executionStatus.status.active_publication_generation',
    ),
    graphRevision: stringAt(
      status.graph_revision,
      'executionStatus.status.graph_revision',
    ),
    retainedBundleId: stringAt(
      status.retained_bundle_id,
      'executionStatus.status.retained_bundle_id',
    ),
    sceneRevision: sceneRevisionAt(
      status.scene_revision,
      'executionStatus.status.scene_revision',
    ),
    components,
    directControl: motion?.directControl ?? null,
    directControlGripper,
    directControlTargets,
    directControlGripperTargets,
    observedMotion: motion?.observedMotion ?? null,
    observedGripper,
    observedMotions: motionEntries
      .map((entry) => entry.observedMotion)
      .filter((entry): entry is ObservedMotionTarget => entry !== null),
    observedGrippers,
    manualOperations: [],
    runtimeScaleControl: null,
    cellStopAction: {
      available: cellStopAvailable,
      unavailableReason: cellStopUnavailableReason,
      expectedStopGeneration: integerAt(
        cellStop.expected_stop_generation,
        'executionStatus.status.cell_stop_action.expected_stop_generation',
      ),
      latestTerminal,
      fault: cellStopFault,
    },
  };
}

/** Components listed by the owner, including present but ineligible ones. */
function executionComponentsAt(value: unknown, path: string): ExecutionComponentStatus[] {
  const components = boundedArrayAt(value, path, 128).map((entry, index) => {
    const rowPath = `${path}[${index}]`;
    const row = objectAt(entry, rowPath);
    exactKeysAt(row, [
      'command_eligible', 'controllable_component_id', 'reason', 'unsupported_targets',
    ], rowPath);
    const commandEligible = booleanAt(row.command_eligible, `${rowPath}.command_eligible`);
    const unavailableReason = nullableStringAt(row.reason, `${rowPath}.reason`);
    if (commandEligible !== (unavailableReason === null)) {
      return fail(`${rowPath}.reason`, 'a refusal reason exactly when ineligible');
    }
    return {
      controllableComponentId: stringAt(
        row.controllable_component_id,
        `${rowPath}.controllable_component_id`,
      ),
      commandEligible,
      unavailableReason,
      unsupportedTargets: boundedArrayAt(
        row.unsupported_targets,
        `${rowPath}.unsupported_targets`,
        128,
      ).map((target, targetIndex) => {
        const targetPath = `${rowPath}.unsupported_targets[${targetIndex}]`;
        const record = objectAt(target, targetPath);
        exactKeysAt(record, ['command_target_id', 'operation_capability_id'], targetPath);
        return {
          commandTargetId: stringAt(record.command_target_id, `${targetPath}.command_target_id`),
          operationCapabilityId: stringAt(
            record.operation_capability_id,
            `${targetPath}.operation_capability_id`,
          ),
        };
      }),
    };
  });
  const ids = components.map((component) => component.controllableComponentId);
  if (new Set(ids).size !== ids.length
    || ids.some((id, index) => index > 0 && ids[index - 1] >= id)) {
    return fail(path, 'unique components sorted by id');
  }
  return components;
}

function poseAt(value: unknown, path: string): MotionPreviewReceipt['finalTcpPose'] {
  const pose = objectAt(value, path);
  const position = objectAt(pose.position_m, `${path}.position_m`);
  const orientation = objectAt(pose.orientation, `${path}.orientation`);
  exactKeysAt(pose, ['orientation', 'position_m'], path);
  exactKeysAt(position, ['x_m', 'y_m', 'z_m'], `${path}.position_m`);
  exactKeysAt(orientation, ['w', 'x', 'y', 'z'], `${path}.orientation`);
  return {
    position_m: {
      x_m: numberAt(position.x_m, `${path}.position_m.x_m`),
      y_m: numberAt(position.y_m, `${path}.position_m.y_m`),
      z_m: numberAt(position.z_m, `${path}.position_m.z_m`),
    },
    orientation: {
      x: numberAt(orientation.x, `${path}.orientation.x`),
      y: numberAt(orientation.y, `${path}.orientation.y`),
      z: numberAt(orientation.z, `${path}.orientation.z`),
      w: numberAt(orientation.w, `${path}.orientation.w`),
    },
  };
}

export function parseManualControlAvailability(value: unknown): ManualControlAvailability {
  const path = 'manualControlAvailability';
  const envelope = objectAt(value, path);
  exactKeysAt(envelope, ['api_version', 'command', 'ok', 'request_id',
    'request_owner_epoch', 'intent_digest', 'availability', 'manual_handover'], path);
  if (envelope.api_version !== 'manyforge.execution_command_response.v1'
    || envelope.command !== 'read_manual_handover' || envelope.ok !== true) {
    return fail(path, 'a successful manual availability response');
  }
  const flags = objectAt(envelope.availability, `${path}.availability`);
  exactKeysAt(flags, ['available', 'unavailable_reason', 'cleanup_required'], `${path}.availability`);
  const available = booleanAt(flags.available, `${path}.available`);
  const unavailableReason = nullableStringAt(flags.unavailable_reason, `${path}.unavailable_reason`);
  const cleanupRequired = booleanAt(flags.cleanup_required, `${path}.cleanup_required`);
  let manualHandover: ManualProgramHandoverContext | null = null;
  if (envelope.manual_handover !== null) {
    const context = objectAt(envelope.manual_handover, `${path}.manual_handover`);
    exactKeysAt(context, ['program_runner_owner_epoch', 'program_run_id',
      'claim_generation', 'selection_revision'], `${path}.manual_handover`);
    const selection = integerAt(context.selection_revision, `${path}.selection_revision`);
    manualHandover = {
      program_runner_owner_epoch: stringAt(context.program_runner_owner_epoch, `${path}.program_runner_owner_epoch`),
      program_run_id: stringAt(context.program_run_id, `${path}.program_run_id`),
      claim_generation: positiveIntegerAt(context.claim_generation, `${path}.claim_generation`),
      selection_revision: selection,
    };
  }
  if (available === (unavailableReason !== null)
    || (cleanupRequired && (!available || manualHandover === null))) {
    return fail(path, 'consistent availability, reason and cleanup context');
  }
  return {
    requestId: stringAt(envelope.request_id, `${path}.request_id`),
    requestOwnerEpoch: stringAt(envelope.request_owner_epoch, `${path}.request_owner_epoch`),
    intentDigest: digestAt(envelope.intent_digest, `${path}.intent_digest`),
    available, unavailableReason, cleanupRequired, manualHandover,
  };
}

/**
 * The advisory node-correction preview, exactly for the request that asked.
 * A late or foreign reply is refused rather than shown for a newer proposal.
 */
export function parseProgramNodeCorrectionPreview(
  value: unknown,
  request: ProgramNodeCorrectionPreviewRequest,
): ProgramNodeCorrectionPreview {
  const path = 'programNodeCorrectionPreview';
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'request_id', 'program_run_id', 'program_id',
    'program_revision', 'program_binding_id', 'node_instance_id',
    'node_attempt_id', 'program_runner_owner_epoch', 'run_revision',
    'retained_bundle_id', 'graph_revision', 'node_id', 'proposed_paths',
    'parameters_digest', 'target', 'unavailable_reason', 'preview_check',
    'preview',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.program_node_correction_preview.v1') {
    return fail(`${path}.schema_version`, 'manyforge.program_node_correction_preview.v1');
  }
  const proposedPaths = stringArrayAt(record.proposed_paths, `${path}.proposed_paths`);
  if (
    record.request_id !== request.request_id
    || record.program_run_id !== request.program_run_id
    || record.program_id !== request.program_id
    || record.program_revision !== request.program_revision
    || record.program_binding_id !== request.program_binding_id
    || record.node_instance_id !== request.node_instance_id
    || record.node_attempt_id !== request.node_attempt_id
    || record.program_runner_owner_epoch !== request.expected_owner_epoch
    || proposedPaths.length !== request.proposed_inputs.length
    || proposedPaths.some((item, index) => item !== request.proposed_inputs[index].path)
  ) {
    return fail(path, 'the identities of the requested preview');
  }
  let target: ProgramNodeCorrectionPreview['target'] = null;
  if (record.target !== null) {
    const row = objectAt(record.target, `${path}.target`);
    exactKeysAt(row, [
      'target_role_id', 'component_role_id', 'controllable_component_id',
      'command_target_id',
    ], `${path}.target`);
    target = {
      targetRoleId: stringAt(row.target_role_id, `${path}.target.target_role_id`),
      componentRoleId: stringAt(row.component_role_id, `${path}.target.component_role_id`),
      controllableComponentId: stringAt(
        row.controllable_component_id,
        `${path}.target.controllable_component_id`,
      ),
      commandTargetId: stringAt(row.command_target_id, `${path}.target.command_target_id`),
    };
  }
  let unavailableReason: ProgramNodeCorrectionPreview['unavailableReason'] = null;
  if (record.unavailable_reason !== null) {
    const reason = objectAt(record.unavailable_reason, `${path}.unavailable_reason`);
    exactKeysAt(reason, ['code', 'message'], `${path}.unavailable_reason`);
    unavailableReason = {
      code: nonEmptyStringAt(reason.code, `${path}.unavailable_reason.code`),
      message: stringAt(reason.message, `${path}.unavailable_reason.message`),
    };
  }
  const previewCheck = nullableStringAt(record.preview_check, `${path}.preview_check`);
  if (previewCheck !== null && previewCheck !== 'trajectory_planned_and_collision_validated') {
    return fail(`${path}.preview_check`, 'a declared preview check');
  }
  let preview: ProgramNodeCorrectionPreview['preview'] = null;
  if (record.preview !== null) {
    const envelope = objectAt(record.preview, `${path}.preview`);
    if (envelope.ok === true) {
      const receipt = parseMotionPreviewResponse(envelope);
      if (
        receipt.requestId !== request.request_id
        || receipt.programRunId !== request.program_run_id
        || receipt.programBindingId !== request.program_binding_id
        || target === null
        || receipt.componentRoleId !== target.componentRoleId
        || receipt.targetRoleId !== target.targetRoleId
      ) {
        return fail(`${path}.preview`, 'a receipt for the previewed node target');
      }
      preview = { ok: true, receipt };
    } else {
      const error = parseApiErrorDto({ detail: envelope.error });
      if (envelope.ok !== false || error === null) {
        return fail(`${path}.preview`, 'a native preview result or rejection');
      }
      preview = { ok: false, error };
    }
  }
  if (
    (unavailableReason === null) === (preview === null)
    || (preview === null) !== (previewCheck === null)
  ) {
    return fail(path, 'a preview result or an unavailability reason');
  }
  return {
    schemaVersion,
    requestId: request.request_id,
    programRunId: request.program_run_id,
    programId: request.program_id,
    programRevision: request.program_revision,
    programBindingId: request.program_binding_id,
    nodeInstanceId: request.node_instance_id,
    nodeAttemptId: request.node_attempt_id,
    programRunnerOwnerEpoch: request.expected_owner_epoch,
    runRevision: positiveIntegerAt(record.run_revision, `${path}.run_revision`),
    retainedBundleId: digestAt(record.retained_bundle_id, `${path}.retained_bundle_id`),
    graphRevision: digestAt(record.graph_revision, `${path}.graph_revision`),
    nodeId: nullableStringAt(record.node_id, `${path}.node_id`),
    proposedPaths,
    parametersDigest: record.parameters_digest === null
      ? null
      : digestAt(record.parameters_digest, `${path}.parameters_digest`),
    target,
    unavailableReason,
    previewCheck: previewCheck as ProgramNodeCorrectionPreview['previewCheck'],
    preview,
  };
}

export function parseMotionPreviewResponse(value: unknown): MotionPreviewReceipt {
  const envelope = objectAt(value, 'motionPreview');
  if (
    booleanAt(envelope.ok, 'motionPreview.ok') !== true
    || stringAt(envelope.command, 'motionPreview.command') !== 'preview_motion'
  ) {
    return fail('motionPreview', 'a successful preview response');
  }
  const receipt = objectAt(
    envelope.preview_receipt,
    'motionPreview.preview_receipt',
  );
  exactKeysAt(receipt, [
    'collision_world_revision',
    'commit_eligible',
    'component_role_id',
    'final_positions_rad',
    'final_tcp_pose',
    'graph_revision',
    'intent_digest',
    'program_binding_id',
    'program_id',
    'program_revision',
    'program_run_id',
    'receipt_digest',
    'request_id',
    'request_owner_epoch',
    'required_capability_id',
    'retained_bundle_id',
    'scene_revision',
    'schema_version',
    'source_joint_names',
    'start_positions_rad',
    'target_role_id',
  ], 'motionPreview.preview_receipt');
  const schemaVersion = stringAt(
    receipt.schema_version,
    'motionPreview.preview_receipt.schema_version',
  );
  if (schemaVersion !== 'manyforge.direct_control_motion_preview.v1') {
    return fail(
      'motionPreview.preview_receipt.schema_version',
      'manyforge.direct_control_motion_preview.v1',
    );
  }
  const sourceJointNames = stringArrayAt(
    receipt.source_joint_names,
    'motionPreview.preview_receipt.source_joint_names',
  );
  const startPositionsRad = arrayAt(
    receipt.start_positions_rad,
    'motionPreview.preview_receipt.start_positions_rad',
  ).map((item, index) => numberAt(
    item,
    `motionPreview.preview_receipt.start_positions_rad[${index}]`,
  ));
  const finalPositionsRad = arrayAt(
    receipt.final_positions_rad,
    'motionPreview.preview_receipt.final_positions_rad',
  ).map((item, index) => numberAt(
    item,
    `motionPreview.preview_receipt.final_positions_rad[${index}]`,
  ));
  if (
    sourceJointNames.length === 0
    || new Set(sourceJointNames).size !== sourceJointNames.length
    || startPositionsRad.length !== sourceJointNames.length
    || finalPositionsRad.length !== sourceJointNames.length
  ) {
    return fail(
      'motionPreview.preview_receipt',
      'equal non-empty unique joint arrays',
    );
  }
  const collision = objectAt(
    receipt.collision_world_revision,
    'motionPreview.preview_receipt.collision_world_revision',
  );
  exactKeysAt(
    collision,
    ['content_hash', 'revision_id'],
    'motionPreview.preview_receipt.collision_world_revision',
  );
  exactKeysAt(
    objectAt(receipt.scene_revision, 'motionPreview.preview_receipt.scene_revision'),
    ['content_hash', 'revision_id', 'scene_id'],
    'motionPreview.preview_receipt.scene_revision',
  );
  const collisionRevisionId = integerAt(
    collision.revision_id,
    'motionPreview.preview_receipt.collision_world_revision.revision_id',
  );
  if (collisionRevisionId < 1) {
    return fail(
      'motionPreview.preview_receipt.collision_world_revision.revision_id',
      'positive',
    );
  }
  if (booleanAt(
    receipt.commit_eligible,
    'motionPreview.preview_receipt.commit_eligible',
  ) !== true) {
    return fail(
      'motionPreview.preview_receipt.commit_eligible',
      'true',
    );
  }
  return {
    schemaVersion,
    receiptDigest: stringAt(
      receipt.receipt_digest,
      'motionPreview.preview_receipt.receipt_digest',
    ),
    intentDigest: stringAt(
      receipt.intent_digest,
      'motionPreview.preview_receipt.intent_digest',
    ),
    requestId: stringAt(
      receipt.request_id,
      'motionPreview.preview_receipt.request_id',
    ),
    requestOwnerEpoch: stringAt(
      receipt.request_owner_epoch,
      'motionPreview.preview_receipt.request_owner_epoch',
    ),
    programId: stringAt(
      receipt.program_id,
      'motionPreview.preview_receipt.program_id',
    ),
    programRevision: stringAt(
      receipt.program_revision,
      'motionPreview.preview_receipt.program_revision',
    ),
    programBindingId: stringAt(
      receipt.program_binding_id,
      'motionPreview.preview_receipt.program_binding_id',
    ),
    programRunId: stringAt(
      receipt.program_run_id,
      'motionPreview.preview_receipt.program_run_id',
    ),
    graphRevision: stringAt(
      receipt.graph_revision,
      'motionPreview.preview_receipt.graph_revision',
    ),
    retainedBundleId: stringAt(
      receipt.retained_bundle_id,
      'motionPreview.preview_receipt.retained_bundle_id',
    ),
    sceneRevision: sceneRevisionAt(
      receipt.scene_revision,
      'motionPreview.preview_receipt.scene_revision',
    ),
    componentRoleId: stringAt(
      receipt.component_role_id,
      'motionPreview.preview_receipt.component_role_id',
    ),
    targetRoleId: stringAt(
      receipt.target_role_id,
      'motionPreview.preview_receipt.target_role_id',
    ),
    requiredCapabilityId: stringAt(
      receipt.required_capability_id,
      'motionPreview.preview_receipt.required_capability_id',
    ),
    sourceJointNames,
    startPositionsRad,
    finalPositionsRad,
    finalTcpPose: poseAt(
      receipt.final_tcp_pose,
      'motionPreview.preview_receipt.final_tcp_pose',
    ),
    collisionWorldRevision: {
      revision_id: collisionRevisionId,
      content_hash: stringAt(
        collision.content_hash,
        'motionPreview.preview_receipt.collision_world_revision.content_hash',
      ),
    },
    commitEligible: true,
  };
}

export function parseCapabilityCommandPreviewResponse(
  value: unknown,
): CapabilityCommandPreviewReceipt {
  const envelope = objectAt(value, 'capabilityCommandPreview');
  if (
    booleanAt(envelope.ok, 'capabilityCommandPreview.ok') !== true
    || stringAt(
      envelope.command,
      'capabilityCommandPreview.command',
    ) !== 'preview_capability_command'
  ) {
    return fail(
      'capabilityCommandPreview',
      'a successful capability-command preview response',
    );
  }
  const receipt = objectAt(
    envelope.preview_receipt,
    'capabilityCommandPreview.preview_receipt',
  );
  exactKeysAt(receipt, [
    'command_schema_id',
    'command_schema_revision',
    'commit_eligible',
    'component_role_id',
    'coordinate_kind',
    'graph_revision',
    'intent_digest',
    'max_position',
    'min_position',
    'program_binding_id',
    'program_id',
    'program_revision',
    'program_run_id',
    'receipt_digest',
    'request_id',
    'request_owner_epoch',
    'required_capability_id',
    'retained_bundle_id',
    'scene_revision',
    'schema_version',
    'source_coordinate_name',
    'source_kind',
    'target_position',
    'target_role_id',
  ], 'capabilityCommandPreview.preview_receipt');
  const schemaVersion = stringAt(
    receipt.schema_version,
    'capabilityCommandPreview.preview_receipt.schema_version',
  );
  if (
    schemaVersion
    !== 'manyforge.direct_control_capability_command_preview.v1'
  ) {
    return fail(
      'capabilityCommandPreview.preview_receipt.schema_version',
      'manyforge.direct_control_capability_command_preview.v1',
    );
  }
  const coordinateKind = stringAt(
    receipt.coordinate_kind,
    'capabilityCommandPreview.preview_receipt.coordinate_kind',
  );
  if (coordinateKind !== 'angular_rad' && coordinateKind !== 'linear_m') {
    return fail(
      'capabilityCommandPreview.preview_receipt.coordinate_kind',
      'angular_rad or linear_m',
    );
  }
  const sourceKind = stringAt(
    receipt.source_kind,
    'capabilityCommandPreview.preview_receipt.source_kind',
  );
  if (sourceKind !== 'model_role' && sourceKind !== 'component_local') {
    return fail(
      'capabilityCommandPreview.preview_receipt.source_kind',
      'model_role or component_local',
    );
  }
  const minPosition = nullableNumberAt(
    receipt.min_position,
    'capabilityCommandPreview.preview_receipt.min_position',
  );
  const maxPosition = nullableNumberAt(
    receipt.max_position,
    'capabilityCommandPreview.preview_receipt.max_position',
  );
  const targetPosition = numberAt(
    receipt.target_position,
    'capabilityCommandPreview.preview_receipt.target_position',
  );
  if (
    (sourceKind === 'model_role'
      && (minPosition === null
        || maxPosition === null
        || minPosition > targetPosition
        || targetPosition > maxPosition))
    || (sourceKind === 'component_local'
      && (minPosition !== null || maxPosition !== null))
    || booleanAt(
      receipt.commit_eligible,
      'capabilityCommandPreview.preview_receipt.commit_eligible',
    ) !== true
  ) {
    return fail(
      'capabilityCommandPreview.preview_receipt',
      'an in-limit commit-eligible command',
    );
  }
  exactKeysAt(
    objectAt(
      receipt.scene_revision,
      'capabilityCommandPreview.preview_receipt.scene_revision',
    ),
    ['content_hash', 'revision_id', 'scene_id'],
    'capabilityCommandPreview.preview_receipt.scene_revision',
  );
  return {
    schemaVersion,
    receiptDigest: stringAt(
      receipt.receipt_digest,
      'capabilityCommandPreview.preview_receipt.receipt_digest',
    ),
    intentDigest: stringAt(
      receipt.intent_digest,
      'capabilityCommandPreview.preview_receipt.intent_digest',
    ),
    requestId: stringAt(
      receipt.request_id,
      'capabilityCommandPreview.preview_receipt.request_id',
    ),
    requestOwnerEpoch: stringAt(
      receipt.request_owner_epoch,
      'capabilityCommandPreview.preview_receipt.request_owner_epoch',
    ),
    programId: stringAt(
      receipt.program_id,
      'capabilityCommandPreview.preview_receipt.program_id',
    ),
    programRevision: stringAt(
      receipt.program_revision,
      'capabilityCommandPreview.preview_receipt.program_revision',
    ),
    programBindingId: stringAt(
      receipt.program_binding_id,
      'capabilityCommandPreview.preview_receipt.program_binding_id',
    ),
    programRunId: stringAt(
      receipt.program_run_id,
      'capabilityCommandPreview.preview_receipt.program_run_id',
    ),
    graphRevision: stringAt(
      receipt.graph_revision,
      'capabilityCommandPreview.preview_receipt.graph_revision',
    ),
    retainedBundleId: stringAt(
      receipt.retained_bundle_id,
      'capabilityCommandPreview.preview_receipt.retained_bundle_id',
    ),
    sceneRevision: sceneRevisionAt(
      receipt.scene_revision,
      'capabilityCommandPreview.preview_receipt.scene_revision',
    ),
    componentRoleId: stringAt(
      receipt.component_role_id,
      'capabilityCommandPreview.preview_receipt.component_role_id',
    ),
    targetRoleId: stringAt(
      receipt.target_role_id,
      'capabilityCommandPreview.preview_receipt.target_role_id',
    ),
    requiredCapabilityId: stringAt(
      receipt.required_capability_id,
      'capabilityCommandPreview.preview_receipt.required_capability_id',
    ),
    commandSchemaId: stringAt(
      receipt.command_schema_id,
      'capabilityCommandPreview.preview_receipt.command_schema_id',
    ),
    commandSchemaRevision: stringAt(
      receipt.command_schema_revision,
      'capabilityCommandPreview.preview_receipt.command_schema_revision',
    ),
    sourceCoordinateName: stringAt(
      receipt.source_coordinate_name,
      'capabilityCommandPreview.preview_receipt.source_coordinate_name',
    ),
    sourceKind: sourceKind as ResourceSourceKind,
    coordinateKind: coordinateKind as CoordinateKind,
    targetPosition,
    minPosition,
    maxPosition,
    commitEligible: true,
  };
}

const OPERATION_STATES = new Set<OperationStateName>([
  'queued',
  'resolving',
  'planning',
  'validating',
  'prepared',
  'dispatching',
  'executing',
  'stopping',
  'completed',
  'stopped',
  'failed',
  'interrupted',
  'intervention_required',
  'cancelled_pre_dispatch',
]);

const OUTPUT_EVIDENCE = new Set<OutputEvidence>([
  'not_submitted', 'submitted', 'source_accepted', 'source_rejected', 'acceptance_unknown',
]);

/**
 * One participant's binding identity and optional correlated output evidence
 * (SX-2119): a provider participant has both control-binding fields null and a
 * real capability-provider binding; evidence is omitted when absent and an
 * explicit null or unknown spelling is invalid. Evidence is the source's
 * acceptance, never the physical effect.
 */
export function participantOutputEvidenceAt(
  participant: Record<string, unknown>,
  path: string,
): OutputEvidence | undefined {
  const text = (key: string) => typeof participant[key] === 'string' && participant[key] !== '';
  if ('control_binding_id' in participant || 'control_binding_revision' in participant) {
    if (participant.control_binding_id === null && participant.control_binding_revision === null) {
      if (!text('capability_provider_binding_id') || !text('capability_provider_binding_revision')) {
        return fail(path, 'a provider participant with its capability-provider binding');
      }
    } else if (!text('control_binding_id') || !text('control_binding_revision')) {
      return fail(path, 'a paired control binding or both control-binding fields null');
    }
  }
  if (!('output_evidence' in participant)) return undefined;
  const evidence = participant.output_evidence;
  if (typeof evidence !== 'string' || !OUTPUT_EVIDENCE.has(evidence as OutputEvidence)) {
    return fail(`${path}.output_evidence`, 'a known output evidence value');
  }
  return evidence as OutputEvidence;
}

export function parseOperationObservation(value: unknown): OperationObservation {
  const envelope = objectAt(value, 'operationObservation');
  if (
    booleanAt(envelope.ok, 'operationObservation.ok') !== true
    || stringAt(envelope.command, 'operationObservation.command') !== 'read'
  ) {
    return fail('operationObservation', 'a successful read response');
  }
  const operation = objectAt(
    envelope.operation,
    'operationObservation.operation',
  );
  const stateValue = stringAt(
    operation.state,
    'operationObservation.operation.state',
  ) as OperationStateName;
  if (!OPERATION_STATES.has(stateValue)) {
    return fail('operationObservation.operation.state', 'a known state');
  }
  const participants = arrayAt(
    operation.participants,
    'operationObservation.operation.participants',
  );
  if (participants.length !== 1) {
    return fail(
      'operationObservation.operation.participants',
      'one participant',
    );
  }
  const participant = objectAt(
    participants[0],
    'operationObservation.operation.participants[0]',
  );
  const progress = numberAt(
    participant.progress,
    'operationObservation.operation.participants[0].progress',
  );
  if (progress < 0 || progress > 1) {
    return fail(
      'operationObservation.operation.participants[0].progress',
      'within [0, 1]',
    );
  }
  let terminalError: string | null = null;
  if (participant.terminal_error !== null) {
    const error = objectAt(
      participant.terminal_error,
      'operationObservation.operation.participants[0].terminal_error',
    );
    terminalError = stringAt(
      error.message,
      'operationObservation.operation.participants[0].terminal_error.message',
    );
  }
  const revision = integerAt(
    operation.operation_revision,
    'operationObservation.operation.operation_revision',
  );
  if (revision < 1) {
    return fail('operationObservation.operation.operation_revision', 'positive');
  }
  const outputEvidence = participantOutputEvidenceAt(
    participant,
    'operationObservation.operation.participants[0]',
  );
  const optionalParticipantText = (key: string): string | undefined => (
    typeof participant[key] === 'string' && participant[key] !== ''
      ? participant[key] as string
      : undefined
  );
  return {
    operationId: stringAt(
      operation.operation_id,
      'operationObservation.operation.operation_id',
    ),
    operationRevision: revision,
    resident: booleanAt(
      envelope.resident,
      'operationObservation.resident',
    ),
    state: stateValue,
    progress,
    activeStopId: nullableStringAt(
      operation.active_stop_id,
      'operationObservation.operation.active_stop_id',
    ),
    activeStopGeneration: operation.active_stop_generation === null
      ? null
      : integerAt(
        operation.active_stop_generation,
        'operationObservation.operation.active_stop_generation',
    ),
    terminalError,
    componentRoleId: optionalParticipantText('component_role_id'),
    targetRoleId: optionalParticipantText('target_role_id'),
    controllableComponentId: optionalParticipantText('controllable_component_id'),
    commandTargetId: optionalParticipantText('command_target_id'),
    ...(outputEvidence === undefined ? {} : { outputEvidence }),
  };
}

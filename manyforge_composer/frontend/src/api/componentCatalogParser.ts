import type {
  ComponentCatalog,
  ComponentCatalogEntry,
  ComponentCatalogKind,
  ComponentModelPayload,
  ComponentProfileFacts,
  ComponentTcpTarget,
  MachineCatalogEntry,
} from '../types/componentCatalog';
import {
  allowedKeysAt,
  arrayAt,
  booleanAt,
  exactKeysAt,
  fail,
  jsonRecordAt,
  nullableStringAt,
  numberAt,
  objectAt,
  stringAt,
  vector3At,
} from './parsers/shared';

const KINDS: readonly ComponentCatalogKind[] = ['robot', 'tool', 'io_device', 'pose_source', 'ai_model', 'sensor'];

function strings(value: unknown, path: string): string[] {
  return arrayAt(value, path).map((item, index) => stringAt(item, `${path}[${index}]`));
}

function parseEntry(value: unknown, path: string): ComponentCatalogEntry {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'entryId', 'kind', 'componentId', 'componentProfileId', 'modelFamilyId',
    'tags', 'lane', 'description', 'sources', 'components',
  ], path);
  const kind = stringAt(record.kind, `${path}.kind`);
  if (!KINDS.includes(kind as ComponentCatalogKind)) {
    return fail(`${path}.kind`, KINDS.join(', '));
  }
  return {
    entryId: stringAt(record.entryId, `${path}.entryId`),
    kind: kind as ComponentCatalogKind,
    componentId: stringAt(record.componentId, `${path}.componentId`),
    componentProfileId: stringAt(record.componentProfileId, `${path}.componentProfileId`),
    modelFamilyId: nullableStringAt(record.modelFamilyId, `${path}.modelFamilyId`),
    tags: strings(record.tags, `${path}.tags`),
    lane: nullableStringAt(record.lane, `${path}.lane`),
    description: nullableStringAt(record.description, `${path}.description`),
    sources: strings(record.sources, `${path}.sources`),
    components: arrayAt(record.components, `${path}.components`)
      .map((row, index) => jsonRecordAt(row, `${path}.components[${index}]`)),
  };
}

/** MACHINES-BOUNDARY §5 (amended): `{kind: "machine", model_id, title, lanes[], default_owner_id, owner_row, members[]}`. */
function parseMachineEntry(value: unknown, path: string): MachineCatalogEntry {
  const record = objectAt(value, path);
  exactKeysAt(record, ['kind', 'model_id', 'title', 'lanes', 'default_owner_id', 'owner_row', 'members'], path);
  return {
    modelId: stringAt(record.model_id, `${path}.model_id`),
    title: stringAt(record.title, `${path}.title`),
    lanes: strings(record.lanes, `${path}.lanes`),
    defaultOwnerId: stringAt(record.default_owner_id, `${path}.default_owner_id`),
    ownerRow: jsonRecordAt(record.owner_row, `${path}.owner_row`),
    members: arrayAt(record.members, `${path}.members`).map((item, index) => {
      const memberPath = `${path}.members[${index}]`;
      const member = objectAt(item, memberPath);
      exactKeysAt(member, ['role', 'model_id', 'title', 'row'], memberPath);
      return {
        role: stringAt(member.role, `${memberPath}.role`),
        modelId: stringAt(member.model_id, `${memberPath}.model_id`),
        title: stringAt(member.title, `${memberPath}.title`),
        row: jsonRecordAt(member.row, `${memberPath}.row`),
      };
    }),
  };
}

function isMachineEntry(value: unknown): boolean {
  return typeof value === 'object' && value !== null && (value as { kind?: unknown }).kind === 'machine';
}

function parsePayload(value: unknown, path: string): ComponentModelPayload {
  const record = objectAt(value, path);
  exactKeysAt(record, ['massKg', 'centerOfMassM'], path);
  const massKg = numberAt(record.massKg, `${path}.massKg`);
  if (massKg < 0) return fail(`${path}.massKg`, 'a non-negative number');
  return { massKg, centerOfMassM: vector3At(record.centerOfMassM, `${path}.centerOfMassM`) };
}

function parseTcpTarget(value: unknown, path: string): ComponentTcpTarget {
  const record = objectAt(value, path);
  exactKeysAt(record, ['commandTargetId', 'tcpFrameRoleIds', 'defaultTcpFrameRoleId'], path);
  return {
    commandTargetId: stringAt(record.commandTargetId, `${path}.commandTargetId`),
    tcpFrameRoleIds: strings(record.tcpFrameRoleIds, `${path}.tcpFrameRoleIds`),
    defaultTcpFrameRoleId: stringAt(record.defaultTcpFrameRoleId, `${path}.defaultTcpFrameRoleId`),
  };
}

function parseProfileFacts(value: unknown, path: string): ComponentProfileFacts {
  const record = objectAt(value, path);
  allowedKeysAt(record, ['tags', 'attachmentInterfaces', 'tcpTargets'], ['payload', 'frameTargets', 'hasMotionTarget', 'lanes'], path);
  return {
    tags: strings(record.tags, `${path}.tags`),
    attachmentInterfaces: strings(record.attachmentInterfaces, `${path}.attachmentInterfaces`),
    tcpTargets: arrayAt(record.tcpTargets, `${path}.tcpTargets`)
      .map((target, index) => parseTcpTarget(target, `${path}.tcpTargets[${index}]`)),
    ...(record.hasMotionTarget === undefined ? {} : {
      hasMotionTarget: booleanAt(record.hasMotionTarget, `${path}.hasMotionTarget`),
    }),
    ...(record.frameTargets === undefined ? {} : {
      frameTargets: arrayAt(record.frameTargets, `${path}.frameTargets`).map((value, index) => {
        const targetPath = `${path}.frameTargets[${index}]`;
        const target = objectAt(value, targetPath);
        exactKeysAt(target, ['commandTargetId', 'frameRoleIds'], targetPath);
        return { commandTargetId: stringAt(target.commandTargetId, `${targetPath}.commandTargetId`),
          frameRoleIds: strings(target.frameRoleIds, `${targetPath}.frameRoleIds`) };
      }),
    }),
    ...(record.payload === undefined ? {} : { payload: parsePayload(record.payload, `${path}.payload`) }),
    ...(record.lanes === undefined ? {} : { lanes: strings(record.lanes, `${path}.lanes`) }),
  };
}

export function parseComponentCatalog(value: unknown): ComponentCatalog {
  const path = 'componentCatalog';
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schemaVersion', 'projectRoot', 'entries', 'profilesWithoutEntry', 'componentProfiles',
  ], path);
  if (record.schemaVersion !== 'manyforge.composer_component_catalog.v1') {
    return fail(`${path}.schemaVersion`, 'manyforge.composer_component_catalog.v1');
  }
  return {
    schemaVersion: 'manyforge.composer_component_catalog.v1',
    projectRoot: nullableStringAt(record.projectRoot, `${path}.projectRoot`),
    entries: arrayAt(record.entries, `${path}.entries`).flatMap((entry, index) => (
      isMachineEntry(entry) ? [] : [parseEntry(entry, `${path}.entries[${index}]`)])),
    machines: arrayAt(record.entries, `${path}.entries`).flatMap((entry, index) => (
      isMachineEntry(entry) ? [parseMachineEntry(entry, `${path}.entries[${index}]`)] : [])),
    profilesWithoutEntry: strings(record.profilesWithoutEntry, `${path}.profilesWithoutEntry`),
    componentProfiles: Object.fromEntries(
      Object.entries(objectAt(record.componentProfiles, `${path}.componentProfiles`))
        .map(([profile, facts]) => [profile, parseProfileFacts(facts, `${path}.componentProfiles.${profile}`)]),
    ),
  };
}

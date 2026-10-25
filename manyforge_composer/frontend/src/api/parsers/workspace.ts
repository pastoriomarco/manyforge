import type {
  AuthoringWorkspaceProgramSaved,
  AuthoringWorkspaceProjectFacts,
  AuthoringWorkspaceSnapshot,
  CurrentScenarioResponse,
  DeploymentCandidateOperationResponse,
  DeploymentCandidatePreparation,
  DeploymentWorkspaceResponse,
  ProgramDraftProjection,
  ProgramFormulaReferences,
  ProgramDraftApplicability,
  JsonValue,
  AppliedProgramSelection,
  ProgramAuthoringDocument,
  ProgramAuthoringDiagnostic,
  ProgramValueSource,
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionResponse,
  ScenarioDocumentDisplayStatus,
  ScenarioDocumentExportResponse,
  ScenarioDocumentIndexResponse,
  ScenarioDocumentLoadResponse,
  ScenarioDocumentSaveResponse,
} from '../../types/api';
import type { AuthoringWorkspaceContainment, AuthoringWorkspaceIdentity } from '../../types/headless';
import { parseProgramBindingTransport, ProgramBindingTransportError } from '../programBindingTransport.generated';
import {
  StructuralContractError,
  allowedKeysAt,
  arrayAt,
  booleanAt,
  boundedArrayAt,
  digestAt,
  exactKeysAt,
  fail,
  integerAt,
  jsonRecordAt,
  nonEmptyStringAt,
  nullableStringAt,
  numberAt,
  objectAt,
  positiveIntegerAt,
  projectPathAt,
  stringArrayAt,
  stringAt,
} from './shared';
import { parseProgramAuthoringDocument, parseProgramDocument } from './program';
import {
  parseManagerApplyResponse,
  parseManagerDeploymentCandidatePreviewResponse,
  parsePreparedActivationInput,
  parseRuntimeAvailability,
} from './manager';

export function parseAuthoringSubmissionOutcome(value: unknown, request: {
  requestOwnerEpoch: string; requestId: string; expectedAdmissionGeneration: number;
}): { kind: 'settled' | 'manager_accepted'; apply: { applyId: string; applyGeneration: number } | null } | null {
  const path = 'authoringSubmissionOutcome';
  const row = objectAt(value, path);
  exactKeysAt(row, ['schema_version', 'request_owner_epoch', 'request_id', 'expected_admission_generation', 'owner_epoch', 'outcome'], path);
  if (row.schema_version !== 'manyforge.authoring_submission_outcome.v1'
    || row.request_owner_epoch !== request.requestOwnerEpoch || row.request_id !== request.requestId
    || row.expected_admission_generation !== request.expectedAdmissionGeneration) {
    throw new Error('Authoring submission identity mismatch');
  }
  if (!stringAt(row.owner_epoch, `${path}.owner_epoch`)) fail(path, 'a nonempty owner epoch');
  if (row.outcome === null) return null;
  const result = objectAt(row.outcome, `${path}.outcome`);
  exactKeysAt(result, ['kind', 'operation_kind', 'apply'], `${path}.outcome`);
  if (!stringAt(result.operation_kind, `${path}.outcome.operation_kind`)) fail(path, 'a nonempty operation kind');
  if (result.kind === 'settled' && result.apply === null) return { kind: 'settled', apply: null };
  if (result.kind !== 'manager_accepted') throw new Error('Invalid authoring submission outcome');
  const apply = objectAt(result.apply, `${path}.outcome.apply`);
  exactKeysAt(apply, ['apply_id', 'apply_generation'], `${path}.outcome.apply`);
  if (!stringAt(apply.apply_id, `${path}.outcome.apply.apply_id`)) fail(path, 'a nonempty Apply identity');
  return { kind: 'manager_accepted', apply: {
    applyId: stringAt(apply.apply_id, `${path}.outcome.apply.apply_id`),
    applyGeneration: positiveIntegerAt(apply.apply_generation, `${path}.outcome.apply.apply_generation`),
  } };
}

function equalJson(left: JsonValue, right: JsonValue): boolean {
  if (left === right) return true;
  if (left === null || right === null || typeof left !== typeof right) return false;
  if (Array.isArray(left) || Array.isArray(right)) {
    return Array.isArray(left)
      && Array.isArray(right)
      && left.length === right.length
      && left.every((value, index) => equalJson(value, right[index]!));
  }
  if (typeof left !== 'object' || typeof right !== 'object') return false;
  const leftKeys = Object.keys(left).sort();
  const rightKeys = Object.keys(right).sort();
  return leftKeys.length === rightKeys.length
    && leftKeys.every((key, index) =>
      key === rightKeys[index] && equalJson(left[key]!, right[key]!));
}

function parseProgramAuthoringDiagnostic(
  value: unknown,
  path: string,
): ProgramAuthoringDiagnostic {
  const record = objectAt(value, path);
  exactKeysAt(record, ['code', 'path', 'message', 'severity'], path);
  const severity = stringAt(record.severity, `${path}.severity`);
  if (severity !== 'error') return fail(`${path}.severity`, 'error');
  return {
    code: stringAt(record.code, `${path}.code`),
    path: stringAt(record.path, `${path}.path`),
    message: stringAt(record.message, `${path}.message`),
    severity,
  };
}

/**
 * Parse the Program Saved projection shared by the Program workspace response
 * and the composite authoring snapshot.
 *
 * Both responses must report the same truthful Draft/Saved comparison, so the
 * invariant checks live here once instead of being duplicated per response.
 */
function parseProgramSavedProjection(
  value: unknown,
  path: string,
  draft: ProgramDraftProjection,
): AuthoringWorkspaceProgramSaved {
  const saved = objectAt(value, path);
  exactKeysAt(saved, ['available', 'matchesDraft', 'path', 'programRevision'], path);
  const available = booleanAt(saved.available, `${path}.available`);
  const savedPath = nullableStringAt(saved.path, `${path}.path`);
  const savedRevision = nullableStringAt(
    saved.programRevision,
    `${path}.programRevision`,
  );
  const matchesDraft = booleanAt(saved.matchesDraft, `${path}.matchesDraft`);
  if (available !== (savedPath !== null) || (!available && savedRevision !== null)) {
    return fail(path, 'internally consistent Saved identity');
  }
  if (
    (savedRevision !== null && matchesDraft !== (
      draft.programRevision !== null
      && savedRevision !== null
      && draft.programRevision === savedRevision
    )) || (savedRevision === null && matchesDraft && (!available || draft.candidateAvailable))
  ) {
    return fail(path, 'a truthful Draft/Saved comparison');
  }
  return {
    available,
    matchesDraft,
    path: savedPath,
    programRevision: savedRevision,
  };
}

function formulaNamesAt(value: unknown, path: string): string[] {
  const names = boundedArrayAt(value, path, 512).map((name, index) =>
    stringAt(name, `${path}[${index}]`));
  if (
    names.some((name) => !/^(param|key)\../.test(name))
    || names.some((name, index) => index > 0 && names[index - 1] >= name)
  ) {
    return fail(path, 'sorted unique param. and key. names');
  }
  return names;
}

/** Each reported formula must be one the Draft's authoring holds. */
function parseProgramFormulaReferences(
  value: unknown,
  path: string,
  authoring: ProgramAuthoringDocument | null,
): ProgramFormulaReferences {
  const record = objectAt(value, path);
  exactKeysAt(record, ['computedKeys', 'nodes'], path);
  const formulaKeys = new Set(
    (authoring?.blackboard?.keys ?? [])
      .filter((key) => key.expression !== undefined)
      .map((key) => key.key),
  );
  const computedKeys = Object.fromEntries(
    Object.entries(objectAt(record.computedKeys, `${path}.computedKeys`)).map(
      ([key, names]) => {
        if (!formulaKeys.has(key)) {
          return fail(`${path}.computedKeys.${key}`, 'a computed key of the Draft');
        }
        return [key, formulaNamesAt(names, `${path}.computedKeys.${key}`)];
      },
    ),
  );
  const sources = new Map<string, Record<string, ProgramValueSource>>();
  const pending = [...(authoring?.forest ?? [])];
  for (let node = pending.pop(); node !== undefined; node = pending.pop()) {
    sources.set(node.instance_id, node.value_sources ?? {});
    pending.push(...node.children);
  }
  const nodes = Object.fromEntries(
    Object.entries(objectAt(record.nodes, `${path}.nodes`)).map(
      ([instanceId, formulas]) => [
        instanceId,
        Object.fromEntries(
          Object.entries(objectAt(formulas, `${path}.nodes.${instanceId}`)).map(
            ([valuePath, names]) => {
              const at = `${path}.nodes.${instanceId}.${valuePath}`;
              if (sources.get(instanceId)?.[valuePath]?.kind !== 'expression') {
                return fail(at, 'a formula of a Draft node');
              }
              return [valuePath, formulaNamesAt(names, at)];
            },
          ),
        ),
      ],
    ),
  );
  return { computedKeys, nodes };
}

function parseProgramDraftProjection(
  value: unknown,
  path: string,
): ProgramDraftProjection {
  const draft = objectAt(value, path);
  exactKeysAt(
    draft,
    [
      'draftId',
      'draftRevision',
      'programLoaded',
      'authoring',
      'candidateAvailable',
      'programRevision',
      'program',
      'diagnostics',
      'formulaReferences',
    ],
    path,
  );
  const programLoaded = booleanAt(draft.programLoaded, `${path}.programLoaded`);
  const authoring = draft.authoring === null
    ? null
    : parseProgramAuthoringDocument(draft.authoring, `${path}.authoring`);
  const candidateAvailable = booleanAt(
    draft.candidateAvailable,
    `${path}.candidateAvailable`,
  );
  const program = draft.program === null
    ? null
    : parseProgramDocument(draft.program, `${path}.program`);
  const programRevision = nullableStringAt(
    draft.programRevision,
    `${path}.programRevision`,
  );
  const diagnostics = boundedArrayAt(
    draft.diagnostics,
    `${path}.diagnostics`,
    100_000,
  ).map((value, index) => parseProgramAuthoringDiagnostic(
    value,
    `${path}.diagnostics[${index}]`,
  ));
  if (
    programLoaded !== (authoring !== null)
    || candidateAvailable !== (program !== null)
    || (program?.program_revision ?? null) !== programRevision
    || (candidateAvailable && diagnostics.length !== 0)
    || (programLoaded && !candidateAvailable && diagnostics.length === 0)
  ) {
    return fail(path, 'internally consistent Program identity');
  }
  const draftId = stringAt(draft.draftId, `${path}.draftId`);
  if (!draftId) {
    return fail(`${path}.draftId`, 'a non-empty Draft identity');
  }
  return {
    draftId,
    draftRevision: integerAt(draft.draftRevision, `${path}.draftRevision`),
    programLoaded,
    authoring,
    candidateAvailable,
    programRevision,
    program,
    diagnostics,
    formulaReferences: parseProgramFormulaReferences(
      draft.formulaReferences,
      `${path}.formulaReferences`,
      authoring,
    ),
  };
}

function parseProgramDraftApplicability(
  value: unknown,
  path: string,
): ProgramDraftApplicability {
  const record = objectAt(value, path);
  exactKeysAt(record, ['applyReady', 'unavailableReason'], path);
  const unavailableReason = record.unavailableReason === null
    ? null
    : parseRuntimeAvailability(record.unavailableReason, `${path}.unavailableReason`);
  const applyReady = booleanAt(record.applyReady, `${path}.applyReady`);
  if (applyReady === (unavailableReason !== null)) {
    return fail(path, 'an exact Apply-ready availability reason');
  }
  return { applyReady, unavailableReason };
}

function parseWorkspaceResponseIdentity(value: unknown): AuthoringWorkspaceIdentity {
  const path = 'workspace.authoringWorkspace';
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'workspaceEpoch', 'workspaceRevision', 'programDraftId',
    'programDraftRevision', 'deploymentDraftId', 'deploymentDraftRevision',
  ], path);
  const identity = parseAuthoringWorkspaceIdentity({
    workspace_epoch: record.workspaceEpoch,
    workspace_revision: record.workspaceRevision,
    program_draft_id: record.programDraftId,
    program_draft_revision: record.programDraftRevision,
    deployment_draft_id: record.deploymentDraftId,
    deployment_draft_revision: record.deploymentDraftRevision,
  }, path);
  if (!identity.workspaceEpoch || !identity.programDraftId || !identity.deploymentDraftId
    || identity.programDraftRevision < 0 || identity.deploymentDraftRevision < 0) {
    return fail(path, 'non-empty identities and non-negative Draft revisions');
  }
  return identity;
}

export function parseProgramWorkspaceResponse(value: unknown): ProgramWorkspaceResponse {
  const record = objectAt(value, 'programWorkspace');
  exactKeysAt(
    record,
    [
      'schemaVersion',
      'authoringWorkspace',
      'draft',
      'applicability',
      'saved',
      'applied',
      'hasUnsavedChanges',
      'hasUnappliedChanges',
    ],
    'programWorkspace',
  );
  const schemaVersion = stringAt(record.schemaVersion, 'programWorkspace.schemaVersion');
  if (schemaVersion !== 'manyforge.composer_program_workspace.v2') {
    return fail('programWorkspace.schemaVersion', 'the supported workspace version');
  }
  const draft = parseProgramDraftProjection(record.draft, 'programWorkspace.draft');
  const authoringWorkspace = parseWorkspaceResponseIdentity(record.authoringWorkspace);
  if (authoringWorkspace.programDraftId !== draft.draftId
    || authoringWorkspace.programDraftRevision !== draft.draftRevision) {
    return fail('programWorkspace.authoringWorkspace', 'the exact Program Draft identity');
  }
  const applicability = parseProgramDraftApplicability(
    record.applicability,
    'programWorkspace.applicability',
  );
  const saved = parseProgramSavedProjection(
    record.saved,
    'programWorkspace.saved',
    draft,
  );
  const applied = objectAt(record.applied, 'programWorkspace.applied');
  exactKeysAt(
    applied,
    ['available', 'matchesDraft', 'programRevision', 'unavailableReason'],
    'programWorkspace.applied',
  );
  const appliedAvailable = booleanAt(applied.available, 'programWorkspace.applied.available');
  const unavailableReason = applied.unavailableReason === null
    ? null
    : parseRuntimeAvailability(
        applied.unavailableReason,
        'programWorkspace.applied.unavailableReason',
      );
  if (appliedAvailable === (unavailableReason !== null)) {
    return fail('programWorkspace.applied', 'consistent availability and reason');
  }
  const appliedMatchesDraft = applied.matchesDraft === null
    ? null
    : booleanAt(applied.matchesDraft, 'programWorkspace.applied.matchesDraft');
  const appliedRevision = nullableStringAt(
    applied.programRevision,
    'programWorkspace.applied.programRevision',
  );
  if (
    (appliedAvailable && appliedMatchesDraft === null)
    || (appliedAvailable && appliedRevision === null && appliedMatchesDraft && draft.candidateAvailable)
    || (!appliedAvailable && (appliedMatchesDraft !== null || appliedRevision !== null))
  ) {
    return fail('programWorkspace.applied', 'internally consistent Applied identity');
  }
  const hasUnappliedChanges = record.hasUnappliedChanges === null
    ? null
    : booleanAt(record.hasUnappliedChanges, 'programWorkspace.hasUnappliedChanges');
  if (!appliedAvailable && hasUnappliedChanges !== null) {
    return fail(
      'programWorkspace.hasUnappliedChanges',
      'null while Applied identity is unavailable',
    );
  }
  const hasUnsavedChanges = booleanAt(
    record.hasUnsavedChanges,
    'programWorkspace.hasUnsavedChanges',
  );
  if (hasUnsavedChanges !== (draft.programLoaded && !saved.matchesDraft)) {
    return fail(
      'programWorkspace.hasUnsavedChanges',
      'the exact Draft/Saved comparison',
    );
  }
  return {
    schemaVersion,
    authoringWorkspace,
    draft,
    applicability,
    saved,
    applied: {
      available: appliedAvailable,
      matchesDraft: appliedMatchesDraft,
      programRevision: appliedRevision,
      unavailableReason,
    },
    hasUnsavedChanges,
    hasUnappliedChanges,
  };
}

const SCENARIO_DOCUMENT_STATUSES = [
  'ready',
  'incomplete',
  'unknown',
  'changed',
  'version_changed',
  'unsupported',
] as const;

function parseAuthoringWorkspaceProjectFacts(
  value: unknown,
  path: string,
): AuthoringWorkspaceProjectFacts {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'document_path', 'document_id', 'document_name', 'document_description',
    'source_diagnostics', 'source_document_digest', 'program_role_assignment',
    'source_program_digest', 'source_deployment_digest',
  ], path);
  const roleAssignment = record.program_role_assignment === null
    ? null
    : jsonRecordAt(
        record.program_role_assignment,
        `${path}.program_role_assignment`,
      );
  return {
    documentPath: nullableStringAt(record.document_path, `${path}.document_path`),
    documentId: nullableStringAt(record.document_id, `${path}.document_id`),
    documentName: nullableStringAt(record.document_name, `${path}.document_name`),
    documentDescription: nullableStringAt(
      record.document_description,
      `${path}.document_description`,
    ),
    sourceDiagnostics: boundedArrayAt(
      record.source_diagnostics,
      `${path}.source_diagnostics`,
      4_096,
    ).map((diagnostic, index) => {
      const diagnosticPath = `${path}.source_diagnostics[${index}]`;
      const selected = objectAt(diagnostic, diagnosticPath);
      exactKeysAt(selected, ['code', 'message', 'source_id'], diagnosticPath);
      const code = stringAt(selected.code, `${diagnosticPath}.code`);
      if (code !== 'scenario_source_changed') {
        return fail(`${diagnosticPath}.code`, 'scenario_source_changed');
      }
      return {
        code,
        sourceId: stringAt(selected.source_id, `${diagnosticPath}.source_id`),
        message: stringAt(selected.message, `${diagnosticPath}.message`),
      };
    }),
    sourceDocumentDigest: nullableStringAt(
      record.source_document_digest,
      `${path}.source_document_digest`,
    ),
    programRoleAssignment: roleAssignment,
    sourceProgramDigest: nullableStringAt(
      record.source_program_digest,
      `${path}.source_program_digest`,
    ),
    sourceDeploymentDigest: nullableStringAt(
      record.source_deployment_digest,
      `${path}.source_deployment_digest`,
    ),
  };
}

/**
 * Parse the one atomic composite authoring snapshot.
 *
 * The composite read is the single owner of authoring identity, both Draft
 * projections and the Scenario project facts, so its shape is closed on both
 * sides: the Program slice must carry exactly the Program Draft projection plus
 * its Saved projection, and the Deployment slice exactly its Draft and Saved
 * projection. Applied, Manager and runtime truth are absent by contract and stay
 * in their existing owner projections.
 */
export function parseAuthoringWorkspaceSnapshot(
  value: unknown,
): AuthoringWorkspaceSnapshot {
  const path = 'authoringWorkspace';
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['schemaVersion', 'workspaceEpoch', 'workspaceRevision', 'program', 'deployment', 'project'],
    path,
  );
  const schemaVersion = stringAt(record.schemaVersion, `${path}.schemaVersion`);
  if (schemaVersion !== 'manyforge.composer_authoring_workspace_snapshot.v2') {
    return fail(
      `${path}.schemaVersion`,
      'manyforge.composer_authoring_workspace_snapshot.v2',
    );
  }
  const workspaceEpoch = stringAt(record.workspaceEpoch, `${path}.workspaceEpoch`);
  if (!workspaceEpoch) {
    return fail(`${path}.workspaceEpoch`, 'a non-empty workspace epoch');
  }
  const programRecord = objectAt(record.program, `${path}.program`);
  exactKeysAt(programRecord, [
    'draftId', 'draftRevision', 'programLoaded', 'authoring', 'candidateAvailable',
    'programRevision', 'program', 'diagnostics', 'formulaReferences', 'saved',
  ], `${path}.program`);
  const { saved: savedRecord, ...draftFields } = programRecord;
  const programDraft = parseProgramDraftProjection(
    draftFields,
    `${path}.program`,
  );
  const programSaved = parseProgramSavedProjection(
    savedRecord,
    `${path}.program.saved`,
    programDraft,
  );
  const deploymentRecord = objectAt(record.deployment, `${path}.deployment`);
  exactKeysAt(deploymentRecord, [
    'draftId', 'draftRevision', 'document', 'savedPath', 'savedDocument',
    'differsFromSaved',
  ], `${path}.deployment`);
  const deploymentDraftId = stringAt(
    deploymentRecord.draftId,
    `${path}.deployment.draftId`,
  );
  if (!deploymentDraftId) {
    return fail(`${path}.deployment.draftId`, 'a non-empty Draft identity');
  }
  const document = deploymentRecord.document === null
    ? null
    : jsonRecordAt(deploymentRecord.document, `${path}.deployment.document`);
  const savedDocument = deploymentRecord.savedDocument === null
    ? null
    : jsonRecordAt(
        deploymentRecord.savedDocument,
        `${path}.deployment.savedDocument`,
      );
  const differsFromSaved = booleanAt(
    deploymentRecord.differsFromSaved,
    `${path}.deployment.differsFromSaved`,
  );
  if (
    differsFromSaved
      !== (document !== null && (savedDocument === null || !equalJson(document, savedDocument)))
  ) {
    return fail(`${path}.deployment`, 'a truthful Draft/Saved comparison');
  }
  return {
    schemaVersion,
    workspaceEpoch,
    workspaceRevision: positiveIntegerAt(
      record.workspaceRevision,
      `${path}.workspaceRevision`,
    ),
    program: { draft: programDraft, saved: programSaved },
    deployment: {
      draftId: deploymentDraftId,
      draftRevision: integerAt(
        deploymentRecord.draftRevision,
        `${path}.deployment.draftRevision`,
      ),
      document,
      savedPath: nullableStringAt(
        deploymentRecord.savedPath,
        `${path}.deployment.savedPath`,
      ),
      savedDocument,
      differsFromSaved,
    },
    project: parseAuthoringWorkspaceProjectFacts(record.project, `${path}.project`),
  };
}

export function parseScenarioDocumentIndexResponse(
  value: unknown,
): ScenarioDocumentIndexResponse {
  const path = 'scenarioDocumentIndex';
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['api_version', 'directory', 'entries'],
    path,
  );
  const apiVersion = stringAt(record.api_version, `${path}.api_version`);
  if (apiVersion !== 'manyforge.scenario_document_index.v1') {
    return fail(`${path}.api_version`, 'manyforge.scenario_document_index.v1');
  }
  return {
    apiVersion,
    directory: stringAt(record.directory, `${path}.directory`),
    entries: boundedArrayAt(record.entries, `${path}.entries`, 10_000).map(
      (entry, index) => {
        const entryPath = `${path}.entries[${index}]`;
        const row = objectAt(entry, entryPath);
        exactKeysAt(row, [
          'description',
          'display_status',
          'folder_name',
          'location',
          'name',
          'path',
          'saved_manyforge_version',
          'scenario_id',
          'source_kind',
          'status_detail',
        ], entryPath);
        const sourceKind = stringAt(row.source_kind, `${entryPath}.source_kind`);
        if (sourceKind !== 'saved') {
          return fail(`${entryPath}.source_kind`, 'saved');
        }
        const displayStatus = stringAt(
          row.display_status,
          `${entryPath}.display_status`,
        );
        if (!SCENARIO_DOCUMENT_STATUSES.includes(
          displayStatus as ScenarioDocumentDisplayStatus,
        )) {
          return fail(`${entryPath}.display_status`, 'a Scenario display status');
        }
        return {
          sourceKind,
          path: stringAt(row.path, `${entryPath}.path`),
          scenarioId: nullableStringAt(
            row.scenario_id,
            `${entryPath}.scenario_id`,
          ),
          name: stringAt(row.name, `${entryPath}.name`),
          folderName: stringAt(row.folder_name, `${entryPath}.folder_name`),
          location: stringAt(row.location, `${entryPath}.location`),
          description: stringAt(row.description, `${entryPath}.description`),
          savedManyforgeVersion: nullableStringAt(
            row.saved_manyforge_version,
            `${entryPath}.saved_manyforge_version`,
          ),
          displayStatus: displayStatus as ScenarioDocumentDisplayStatus,
          statusDetail: stringAt(
            row.status_detail,
            `${entryPath}.status_detail`,
          ),
        };
      },
    ),
  };
}

export function parseScenarioDocumentSaveResponse(
  value: unknown,
): ScenarioDocumentSaveResponse {
  const path = 'scenarioDocumentSave';
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['last_known_status', 'path', 'scenario_id', 'status', 'workspace'],
    path,
  );
  const status = stringAt(record.status, `${path}.status`);
  if (status !== 'saved') return fail(`${path}.status`, 'saved');
  const lastKnownStatus = stringAt(
    record.last_known_status,
    `${path}.last_known_status`,
  );
  if (!['ready', 'incomplete', 'unknown'].includes(lastKnownStatus)) {
    return fail(`${path}.last_known_status`, 'ready, incomplete, or unknown');
  }
  return {
    status,
    path: stringAt(record.path, `${path}.path`),
    scenarioId: stringAt(record.scenario_id, `${path}.scenario_id`),
    lastKnownStatus: lastKnownStatus as ScenarioDocumentSaveResponse['lastKnownStatus'],
    workspace: parseProgramWorkspaceResponse(record.workspace),
  };
}

export function parseScenarioDocumentLoadResponse(
  value: unknown,
): ScenarioDocumentLoadResponse {
  const path = 'scenarioDocumentLoad';
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['activation', 'authoringWorkspace', 'diagnostics', 'incomplete', 'path', 'scenario_id', 'source_diagnostics', 'status', 'workspace'],
    path,
  );
  const status = stringAt(record.status, `${path}.status`);
  if (status !== 'accepted' && status !== 'applied') {
    return fail(`${path}.status`, 'accepted or applied');
  }
  const activation = jsonRecordAt(record.activation, `${path}.activation`);
  const activationStatus = stringAt(
    activation.status,
    `${path}.activation.status`,
  );
  if (activationStatus !== status) {
    return fail(
      `${path}.activation.status`,
      `to match Scenario Load status ${status}`,
    );
  }
  const common = {
    path: stringAt(record.path, `${path}.path`),
    scenarioId: stringAt(record.scenario_id, `${path}.scenario_id`),
    incomplete: booleanAt(record.incomplete, `${path}.incomplete`),
    diagnostics: arrayAt(record.diagnostics, `${path}.diagnostics`).map(
      (diagnostic, index) => parseProgramAuthoringDiagnostic(
        diagnostic,
        `${path}.diagnostics[${index}]`,
      ),
    ),
    sourceDiagnostics: arrayAt(record.source_diagnostics, `${path}.source_diagnostics`).map(
      (diagnostic, index) => {
        const diagnosticPath = `${path}.source_diagnostics[${index}]`;
        const selected = objectAt(diagnostic, diagnosticPath);
        exactKeysAt(selected, ['code', 'message', 'source_id'], diagnosticPath);
        const code = stringAt(selected.code, `${diagnosticPath}.code`);
        if (code !== 'scenario_source_changed') {
          return fail(`${diagnosticPath}.code`, 'scenario_source_changed');
        }
        return {
          code: 'scenario_source_changed' as const,
          sourceId: stringAt(selected.source_id, `${diagnosticPath}.source_id`),
          message: stringAt(selected.message, `${diagnosticPath}.message`),
        };
      },
    ),
    workspace: parseProgramWorkspaceResponse(record.workspace),
    authoringWorkspace: parseAuthoringWorkspaceSnapshot(
      record.authoringWorkspace,
    ),
  };
  if (status === 'accepted') {
    return {
      ...common,
      status,
      activation: {
        ...activation,
        status,
        apply_id: nonEmptyStringAt(
          activation.apply_id,
          `${path}.activation.apply_id`,
        ),
        apply_generation: positiveIntegerAt(
          activation.apply_generation,
          `${path}.activation.apply_generation`,
        ),
      },
    };
  }
  if (activation.apply_id !== null) {
    return fail(
      `${path}.activation.apply_id`,
      'null when the Scenario is already Applied',
    );
  }
  return {
    ...common,
    status,
    activation: { ...activation, status, apply_id: null },
  };
}

export function parseScenarioDocumentExportResponse(
  value: unknown,
): ScenarioDocumentExportResponse {
  const path = 'scenarioDocumentExport';
  const record = objectAt(value, path);
  exactKeysAt(record, ['path', 'status'], path);
  const status = stringAt(record.status, `${path}.status`);
  if (status !== 'exported') return fail(`${path}.status`, 'exported');
  return { status, path: stringAt(record.path, `${path}.path`) };
}

export function parseDeploymentWorkspaceResponse(
  value: unknown,
): DeploymentWorkspaceResponse {
  const path = 'deploymentWorkspace';
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schemaVersion', 'draft', 'saved', 'managerApplied', 'runAdmission',
    'binding', 'scenarioCandidates', 'savedScenarioCandidates',
    'unavailableReasons', 'authoringWorkspace',
  ], path);
  const schemaVersion = stringAt(record.schemaVersion, `${path}.schemaVersion`);
  if (schemaVersion !== 'manyforge.composer_deployment_workspace.v1') {
    return fail(`${path}.schemaVersion`, 'the supported Deployment workspace version');
  }
  const draftRecord = objectAt(record.draft, `${path}.draft`);
  exactKeysAt(draftRecord, [
    'id', 'revision', 'document', 'deploymentArtifactId',
    'deploymentRevision', 'graphRevision', 'differsFromSaved',
  ], `${path}.draft`);
  const authoringWorkspace = parseWorkspaceResponseIdentity(record.authoringWorkspace);
  if (authoringWorkspace.deploymentDraftId !== draftRecord.id
    || authoringWorkspace.deploymentDraftRevision !== draftRecord.revision) {
    return fail(`${path}.authoringWorkspace`, 'the exact Deployment Draft identity');
  }
  const document = draftRecord.document === null
    ? null
    : jsonRecordAt(draftRecord.document, `${path}.draft.document`);
  const deploymentArtifactId = nullableStringAt(
    draftRecord.deploymentArtifactId,
    `${path}.draft.deploymentArtifactId`,
  );
  const deploymentRevision = draftRecord.deploymentRevision === null
    ? null
    : digestAt(draftRecord.deploymentRevision, `${path}.draft.deploymentRevision`);
  const graphRevision = draftRecord.graphRevision === null
    ? null
    : digestAt(draftRecord.graphRevision, `${path}.draft.graphRevision`);
  const missingDraftIdentities = [
    deploymentArtifactId,
    deploymentRevision,
    graphRevision,
  ].filter((identity) => identity === null).length;
  const authoredIntent = document?.api_version === 'manyforge.deployment_intent.v2';
  if (
    (document === null && missingDraftIdentities !== 3)
    || (document !== null && !authoredIntent && missingDraftIdentities !== 0)
    || (authoredIntent && (deploymentArtifactId === null
      || deploymentRevision !== null || graphRevision !== null))
  ) {
    return fail(`${path}.draft`, 'complete identities exactly when a document is loaded');
  }
  if (document !== null && authoredIntent) {
    // Source-referenced authoring intent has no compiler-issued identities.
    // Those belong to the prepared activation, never to the mutable Draft.
    allowedKeysAt(document,
      ['api_version', 'deployment_artifact_id', 'name',
        'components', 'attachments', 'policies'],
      ['description', 'assistant_profile_id', 'compatibility'],
      `${path}.draft.document`);
    if (stringAt(document.deployment_artifact_id,
      `${path}.draft.document.deployment_artifact_id`) !== deploymentArtifactId) {
      return fail(`${path}.draft`, 'one exact authored Deployment identity');
    }
    stringAt(document.name, `${path}.draft.document.name`);
    boundedArrayAt(document.components, `${path}.draft.document.components`, 4096);
    boundedArrayAt(document.attachments, `${path}.draft.document.attachments`, 4096);
    objectAt(document.policies, `${path}.draft.document.policies`);
  } else if (document !== null) {
    const apiVersion = stringAt(
      document.api_version,
      `${path}.draft.document.api_version`,
    );
    // This is the normalized authored document, not the compiled cell graph.
    // Its graph revision is the compiler-issued sibling above, joined to the
    // Binding below; authored graph content contains no graph_revision field.
    objectAt(document.graph, `${path}.draft.document.graph`);
    if (
      apiVersion !== 'manyforge.deployment.v1'
      || stringAt(
        document.deployment_artifact_id,
        `${path}.draft.document.deployment_artifact_id`,
      ) !== deploymentArtifactId
      || digestAt(
        document.deployment_revision,
        `${path}.draft.document.deployment_revision`,
      ) !== deploymentRevision
    ) {
      return fail(`${path}.draft`, 'one exact compiled Deployment identity');
    }
  }
  const savedRecord = objectAt(record.saved, `${path}.saved`);
  exactKeysAt(savedRecord, ['path', 'deploymentRevision', 'differsFromApplied'], `${path}.saved`);
  const savedPath = nullableStringAt(savedRecord.path, `${path}.saved.path`);
  const savedRevision = savedRecord.deploymentRevision === null
    ? null
    : digestAt(savedRecord.deploymentRevision, `${path}.saved.deploymentRevision`);
  const savedDiffersFromApplied = savedRecord.differsFromApplied === null
    ? null
    : booleanAt(savedRecord.differsFromApplied, `${path}.saved.differsFromApplied`);
  // A saved source-referenced intent is a file, not a compiled Deployment.
  if (savedPath === null && savedRevision !== null) {
    return fail(`${path}.saved`, 'one complete Saved identity');
  }
  const managerRecord = objectAt(record.managerApplied, `${path}.managerApplied`);
  exactKeysAt(managerRecord, ['activationOrder', 'deploymentRevision', 'scenarioId'], `${path}.managerApplied`);
  const managerRevision = managerRecord.deploymentRevision === null
    ? null
    : digestAt(managerRecord.deploymentRevision, `${path}.managerApplied.deploymentRevision`);
  const managerScenarioId = nullableStringAt(
    managerRecord.scenarioId,
    `${path}.managerApplied.scenarioId`,
  );
  if ((managerRevision === null) !== (managerScenarioId === null)) {
    return fail(`${path}.managerApplied`, 'one complete Manager Applied identity');
  }
  let activationOrder: DeploymentWorkspaceResponse['managerApplied']['activationOrder'] = null;
  if (managerRecord.activationOrder !== null) {
    const orderPath = `${path}.managerApplied.activationOrder`;
    const order = objectAt(managerRecord.activationOrder, orderPath);
    exactKeysAt(order, ['retainedBundleId', 'graphRevision', 'componentIds'], orderPath);
    const componentIds = boundedArrayAt(order.componentIds, `${orderPath}.componentIds`, 4096)
      .map((id, index) => nonEmptyStringAt(id, `${orderPath}.componentIds[${index}]`));
    if (new Set(componentIds).size !== componentIds.length || managerScenarioId === null) {
      return fail(orderPath, 'unique ordered components bound to Manager Applied');
    }
    activationOrder = {
      retainedBundleId: nonEmptyStringAt(order.retainedBundleId, `${orderPath}.retainedBundleId`),
      graphRevision: digestAt(order.graphRevision, `${orderPath}.graphRevision`),
      componentIds,
    };
  }
  const admissionRecord = objectAt(record.runAdmission, `${path}.runAdmission`);
  exactKeysAt(admissionRecord, ['ready', 'reason'], `${path}.runAdmission`);
  const runReady = booleanAt(admissionRecord.ready, `${path}.runAdmission.ready`);
  const runReason = nullableStringAt(admissionRecord.reason, `${path}.runAdmission.reason`);
  if (runReady === (runReason !== null)) {
    return fail(`${path}.runAdmission`, 'a reason exactly while Run is blocked');
  }
  // A compiled Draft revision is compared exactly. A Draft without one (empty,
  // or source-referenced intent) is compared by the backend on documents, so
  // only require that a ready Run names an Applied identity.
  if (
    (deploymentRevision !== null && runReady !== (deploymentRevision === managerRevision))
    || (runReady && managerScenarioId === null)
  ) {
    return fail(
      `${path}.runAdmission`,
      'the exact Deployment Draft and Manager Applied comparison',
    );
  }
  let binding = null;
  if (record.binding !== null) {
    try {
      binding = parseProgramBindingTransport(record.binding, `${path}.binding`);
    } catch (error) {
      if (error instanceof ProgramBindingTransportError) {
        throw new StructuralContractError(error.message);
      }
      throw error;
    }
  }
  const parseScenarioCandidates = (
    value: unknown,
    field: 'scenarioCandidates' | 'savedScenarioCandidates',
  ) => boundedArrayAt(
    value,
    `${path}.${field}`,
    4096,
  ).map((candidate, index) => {
    const candidatePath = `${path}.${field}[${index}]`;
    const row = objectAt(candidate, candidatePath);
    exactKeysAt(row, [
      'scenarioId', 'sourceDocumentDigest', 'deploymentArtifactId',
      'deploymentRevision', 'retainedBundleId', 'programId',
      'programRevision', 'programBindingId', 'description',
    ], candidatePath);
    return {
      scenarioId: stringAt(row.scenarioId, `${candidatePath}.scenarioId`),
      sourceDocumentDigest: digestAt(
        row.sourceDocumentDigest,
        `${candidatePath}.sourceDocumentDigest`,
      ),
      deploymentArtifactId: stringAt(
        row.deploymentArtifactId,
        `${candidatePath}.deploymentArtifactId`,
      ),
      deploymentRevision: digestAt(
        row.deploymentRevision,
        `${candidatePath}.deploymentRevision`,
      ),
      retainedBundleId: digestAt(
        row.retainedBundleId,
        `${candidatePath}.retainedBundleId`,
      ),
      programId: stringAt(row.programId, `${candidatePath}.programId`),
      programRevision: digestAt(
        row.programRevision,
        `${candidatePath}.programRevision`,
      ),
      programBindingId: digestAt(
        row.programBindingId,
        `${candidatePath}.programBindingId`,
      ),
      description: nullableStringAt(row.description, `${candidatePath}.description`),
    };
  });
  const scenarioCandidates = parseScenarioCandidates(
    record.scenarioCandidates,
    'scenarioCandidates',
  );
  const savedScenarioCandidates = parseScenarioCandidates(
    record.savedScenarioCandidates,
    'savedScenarioCandidates',
  );
  const scenarioIds = new Set<string>();
  for (const candidate of scenarioCandidates) {
    if (scenarioIds.has(candidate.scenarioId)) {
      return fail(`${path}.scenarioCandidates`, 'unique Scenario identities');
    }
    scenarioIds.add(candidate.scenarioId);
    if (
      candidate.deploymentArtifactId !== deploymentArtifactId
      || candidate.deploymentRevision !== deploymentRevision
    ) {
      return fail(
        `${path}.scenarioCandidates`,
        'only exact matches for the current Deployment Draft',
      );
    }
  }
  if (savedScenarioCandidates.some(
    (candidate) => candidate.deploymentRevision !== savedRevision,
  )) {
    return fail(
      `${path}.savedScenarioCandidates`,
      'only exact matches for the Saved Deployment',
    );
  }
  if (
    binding !== null
    && (
      binding.graph_revision !== graphRevision
      || deploymentRevision !== managerRevision
    )
  ) {
    return fail(
      `${path}.binding`,
      'the exact current Deployment Draft and graph revision',
    );
  }
  return {
    schemaVersion: 'manyforge.composer_deployment_workspace.v1',
    authoringWorkspace,
    draft: {
      id: stringAt(draftRecord.id, `${path}.draft.id`),
      revision: integerAt(draftRecord.revision, `${path}.draft.revision`),
      document,
      deploymentArtifactId,
      deploymentRevision,
      graphRevision,
      differsFromSaved: booleanAt(
        draftRecord.differsFromSaved,
        `${path}.draft.differsFromSaved`,
      ),
    },
    saved: {
      path: savedPath,
      deploymentRevision: savedRevision,
      differsFromApplied: savedDiffersFromApplied,
    },
    managerApplied: {
      activationOrder,
      deploymentRevision: managerRevision,
      scenarioId: managerScenarioId,
    },
    runAdmission: { ready: runReady, reason: runReason },
    binding,
    scenarioCandidates,
    savedScenarioCandidates,
    unavailableReasons: stringArrayAt(
      record.unavailableReasons,
      `${path}.unavailableReasons`,
    ),
  };
}

export function parseDeploymentCandidateOperationResponse(
  value: unknown,
): DeploymentCandidateOperationResponse {
  const path = 'deploymentCandidateOperation';
  const record = objectAt(value, path);
  exactKeysAt(record, ['schemaVersion', 'workspace', 'result'], path);
  const schemaVersion = stringAt(record.schemaVersion, `${path}.schemaVersion`);
  if (schemaVersion !== 'manyforge.composer_deployment_candidate_operation.v1') {
    return fail(
      `${path}.schemaVersion`,
      'manyforge.composer_deployment_candidate_operation.v1',
    );
  }
  const workspace = parseDeploymentWorkspaceResponse(record.workspace);
  const resultRecord = objectAt(record.result, `${path}.result`);
  allowedKeysAt(resultRecord, [
    'status', 'confirmation_token', 'prepared_activation', 'candidate_preview',
    'manager_apply',
  ], ['preparation', 'phases_ms', 'apply_id', 'apply_generation'], `${path}.result`);
  const status = stringAt(resultRecord.status, `${path}.result.status`);
  if (
    status !== 'previewed'
    && status !== 'confirmation_required'
    && status !== 'accepted'
    && status !== 'applied'
  ) {
    return fail(
      `${path}.result.status`,
      'previewed, confirmation_required, accepted, or applied',
    );
  }
  const confirmationToken = resultRecord.confirmation_token === null
    ? null
    : digestAt(
      resultRecord.confirmation_token,
      `${path}.result.confirmation_token`,
    );
  if ((status === 'confirmation_required') !== (confirmationToken !== null)) {
    return fail(
      `${path}.result.confirmation_token`,
      'a token exactly when confirmation is required',
    );
  }
  if (status === 'confirmation_required'
    && (resultRecord.preparation === undefined
      || resultRecord.preparation === null)) {
    return fail(
      `${path}.result.preparation`,
      'the prepared request identity for a required confirmation',
    );
  }
  let preparation: DeploymentCandidatePreparation | null = null;
  if (resultRecord.preparation !== undefined
    && resultRecord.preparation !== null) {
    const preparationRecord = objectAt(
      resultRecord.preparation,
      `${path}.result.preparation`,
    );
    exactKeysAt(preparationRecord, ['request_owner_epoch', 'request_id'],
      `${path}.result.preparation`);
    preparation = {
      requestOwnerEpoch: stringAt(
        preparationRecord.request_owner_epoch,
        `${path}.result.preparation.request_owner_epoch`,
      ),
      requestId: stringAt(
        preparationRecord.request_id,
        `${path}.result.preparation.request_id`,
      ),
    };
  }
  if (status !== 'confirmation_required' && preparation !== null) {
    return fail(
      `${path}.result.preparation`,
      'null unless confirmation is required',
    );
  }
  const preparedActivation = parsePreparedActivationInput(
    resultRecord.prepared_activation,
    `${path}.result.prepared_activation`,
  );
  // An authored (intent) Draft has no compiled revision of its own: the request's
  // Draft fence already pinned it, so only a compiled Draft must match exactly.
  if (
    workspace.draft.deploymentArtifactId !== preparedActivation.deploymentArtifactId
    || (workspace.draft.deploymentRevision !== null
      && workspace.draft.deploymentRevision !== preparedActivation.deploymentRevision)
  ) {
    return fail(`${path}.workspace`, 'the exact compiled Deployment Draft');
  }
  const candidatePreview = resultRecord.candidate_preview === null
    ? null
    : parseManagerDeploymentCandidatePreviewResponse(
      resultRecord.candidate_preview,
    );
  const managerApply = resultRecord.manager_apply === null
    ? null
    : parseManagerApplyResponse(resultRecord.manager_apply);
  const phaseKeys = [
    'preparation_ms', 'preview_ms', 'confirmation_validation_ms',
  ] as const;
  const phasesRecord = resultRecord.phases_ms === undefined
    ? {}
    : objectAt(resultRecord.phases_ms, `${path}.result.phases_ms`);
  allowedKeysAt(phasesRecord, [], phaseKeys, `${path}.result.phases_ms`);
  const phasesMs: Record<string, number> = {};
  for (const [phase, value] of Object.entries(phasesRecord)) {
    const duration = numberAt(value, `${path}.result.phases_ms.${phase}`);
    if (duration < 0) return fail(
      `${path}.result.phases_ms.${phase}`,
      'a non-negative finite duration',
    );
    phasesMs[phase] = duration;
  }
  const applyId = resultRecord.apply_id === undefined
    ? null
    : nullableStringAt(resultRecord.apply_id, `${path}.result.apply_id`);
  // An accepted Apply names its exact Manager operation, as the Program
  // Apply reply does; no other status carries an Apply generation.
  if (status === 'accepted') {
    if (managerApply === null || applyId !== managerApply.applyId
      || positiveIntegerAt(resultRecord.apply_generation, `${path}.result.apply_generation`)
        !== managerApply.applyGeneration) {
      return fail(`${path}.result`, 'the exact accepted Manager operation');
    }
  } else if (resultRecord.apply_generation !== undefined) {
    return fail(`${path}.result.apply_generation`, 'absent unless the Apply was accepted');
  }
  if (
    candidatePreview !== null
    && (
      candidatePreview.candidateReceipt.deployment_artifact_id
        !== preparedActivation.deploymentArtifactId
      || candidatePreview.candidateReceipt.deployment_revision
        !== preparedActivation.deploymentRevision
      || candidatePreview.candidateReceipt.candidate_retained_bundle_id
        !== preparedActivation.retainedBundleId
      || candidatePreview.candidateReceipt.candidate_graph_revision
        !== preparedActivation.graphRevision
      || candidatePreview.candidateReceipt.prepared_activation_id
        !== preparedActivation.preparedActivationId
    )
  ) {
    return fail(`${path}.result.candidate_preview`, 'the compiled candidate');
  }
  if (
    status === 'applied'
    && workspace.managerApplied.deploymentRevision
      !== preparedActivation.deploymentRevision
  ) {
    return fail(`${path}.workspace.managerApplied`, 'the applied candidate');
  }
  return {
    schemaVersion,
    workspace,
    result: {
      status,
      confirmationToken,
      preparation,
      preparedActivation,
      candidatePreview,
      managerApply,
      phasesMs,
      applyId,
    },
  };
}

export function parseCurrentScenarioResponse(value: unknown): CurrentScenarioResponse {
  const path = 'currentScenario';
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['schema_version', 'ready', 'current_scenario', 'unavailable_reasons'],
    path,
  );
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.current_scenario.v1') {
    return fail(`${path}.schema_version`, 'the supported current-Scenario version');
  }
  const ready = booleanAt(record.ready, `${path}.ready`);
  const unavailableReasons = stringArrayAt(
    record.unavailable_reasons,
    `${path}.unavailable_reasons`,
  );
  const current = record.current_scenario === null
    ? null
    : objectAt(record.current_scenario, `${path}.current_scenario`);
  if (ready !== (current !== null)) {
    return fail(path, 'a current Scenario exactly when ready');
  }
  if (ready !== (unavailableReasons.length === 0)) {
    return fail(path, 'unavailable reasons exactly when no current Scenario exists');
  }
  if (current === null) {
    return {
      schemaVersion: 'manyforge.current_scenario.v1',
      ready,
      currentScenario: null,
      unavailableReasons,
    };
  }
  exactKeysAt(current, [
    'scenario_id',
    'source_document_digest',
    'deployment_artifact_id',
    'deployment_revision',
    'retained_bundle_id',
    'graph_revision',
    'program_id',
    'program_revision',
    'program_binding_id',
  ], `${path}.current_scenario`);
  return {
    schemaVersion: 'manyforge.current_scenario.v1',
    ready,
    currentScenario: {
      scenarioId: stringAt(current.scenario_id, `${path}.current_scenario.scenario_id`),
      sourceDocumentDigest: digestAt(
        current.source_document_digest,
        `${path}.current_scenario.source_document_digest`,
      ),
      deploymentArtifactId: stringAt(
        current.deployment_artifact_id,
        `${path}.current_scenario.deployment_artifact_id`,
      ),
      deploymentRevision: digestAt(
        current.deployment_revision,
        `${path}.current_scenario.deployment_revision`,
      ),
      retainedBundleId: digestAt(
        current.retained_bundle_id,
        `${path}.current_scenario.retained_bundle_id`,
      ),
      graphRevision: digestAt(
        current.graph_revision,
        `${path}.current_scenario.graph_revision`,
      ),
      programId: stringAt(current.program_id, `${path}.current_scenario.program_id`),
      programRevision: digestAt(
        current.program_revision,
        `${path}.current_scenario.program_revision`,
      ),
      programBindingId: digestAt(
        current.program_binding_id,
        `${path}.current_scenario.program_binding_id`,
      ),
    },
    unavailableReasons,
  };
}

export function parseAppliedProgramSelection(value: unknown): AppliedProgramSelection {
  const path = 'appliedProgram';
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'assistant_mode',
      'assistant_projection',
      'baseline_program_binding_id',
      'deployment_artifact_id',
      'deployment_revision',
      'graph_revision',
      'node_environment_digest',
      'owner_identities',
      'preparation_digest',
      'program',
      'program_binding',
      'program_binding_id',
      'program_id',
      'program_revision',
      'project_path',
      'retained_bundle_id',
      'source_document_digest',
      'scenario_id',
      'schema_version',
      'source',
    ],
    path,
  );
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.applied_program_selection.v3') {
    return fail(`${path}.schema_version`, 'manyforge.applied_program_selection.v3');
  }
  const assistantMode = stringAt(record.assistant_mode, `${path}.assistant_mode`);
  if (!['required', 'optional', 'forbidden'].includes(assistantMode)) {
    return fail(`${path}.assistant_mode`, 'required, optional, or forbidden');
  }
  const assistantProjection = record.assistant_projection === null
    ? null
    : jsonRecordAt(record.assistant_projection, `${path}.assistant_projection`);
  const ownerIdentities = boundedArrayAt(
    record.owner_identities,
    `${path}.owner_identities`,
    4096,
  ).map((value, index) => {
    const ownerPath = `${path}.owner_identities[${index}]`;
    const owner = objectAt(value, ownerPath);
    exactKeysAt(owner, ['owner_epoch', 'owner_id', 'owner_role'], ownerPath);
    return {
      ownerRole: stringAt(owner.owner_role, `${ownerPath}.owner_role`),
      ownerId: stringAt(owner.owner_id, `${ownerPath}.owner_id`),
      ownerEpoch: stringAt(owner.owner_epoch, `${ownerPath}.owner_epoch`),
    };
  });
  const ownerRoles = ownerIdentities.map((owner) => owner.ownerRole);
  if (
    ownerRoles.length === 0
    || ownerRoles.some((role, index) => index > 0 && role <= ownerRoles[index - 1])
  ) {
    return fail(
      `${path}.owner_identities`,
      'strictly role-sorted unique owner identities',
    );
  }
  digestAt(
    record.baseline_program_binding_id,
    `${path}.baseline_program_binding_id`,
  );
  stringAt(record.deployment_artifact_id, `${path}.deployment_artifact_id`);
  digestAt(record.deployment_revision, `${path}.deployment_revision`);
  digestAt(record.preparation_digest, `${path}.preparation_digest`);
  const program = parseProgramDocument(record.program, `${path}.program`);
  const bindingPath = `${path}.program_binding`;
  let programBinding;
  try {
    programBinding = parseProgramBindingTransport(record.program_binding, bindingPath);
  } catch (error) {
    if (error instanceof ProgramBindingTransportError) {
      throw new StructuralContractError(error.message);
    }
    throw error;
  }
  const source = objectAt(record.source, `${path}.source`);
  exactKeysAt(
    source,
    [
      'record_digest',
      'semantic_content_digest',
      'source_generation',
      'source_owner_epoch',
      'source_owner_id',
    ],
    `${path}.source`,
  );
  digestAt(
    source.semantic_content_digest,
    `${path}.source.semantic_content_digest`,
  );
  const programId = stringAt(record.program_id, `${path}.program_id`);
  const programRevision = digestAt(
    record.program_revision,
    `${path}.program_revision`,
  );
  const programBindingId = digestAt(
    record.program_binding_id,
    `${path}.program_binding_id`,
  );
  const retainedBundleId = digestAt(
    record.retained_bundle_id,
    `${path}.retained_bundle_id`,
  );
  const graphRevision = digestAt(
    record.graph_revision,
    `${path}.graph_revision`,
  );
  const nodeEnvironmentDigest = digestAt(
    record.node_environment_digest,
    `${path}.node_environment_digest`,
  );
  if (
    program.program_id !== programId
    || program.program_revision !== programRevision
    || programBinding.program_id !== programId
    || programBinding.program_revision !== programRevision
    || programBinding.program_binding_id !== programBindingId
    || programBinding.retained_bundle_id !== retainedBundleId
    || programBinding.graph_revision !== graphRevision
    || programBinding.node_environment.environment_digest !== nodeEnvironmentDigest
  ) {
    return fail(path, 'content-identical Program and Program Binding identities');
  }
  return {
    schemaVersion,
    scenarioId: stringAt(record.scenario_id, `${path}.scenario_id`),
    projectPath: projectPathAt(record.project_path, `${path}.project_path`),
    sourceDocumentDigest: digestAt(
      record.source_document_digest,
      `${path}.source_document_digest`,
    ),
    programId,
    programRevision,
    programBindingId,
    retainedBundleId,
    graphRevision,
    nodeEnvironmentDigest,
    recordDigest: digestAt(source.record_digest, `${path}.source.record_digest`),
    sourceOwnerId: stringAt(source.source_owner_id, `${path}.source.source_owner_id`),
    sourceOwnerEpoch: stringAt(
      source.source_owner_epoch,
      `${path}.source.source_owner_epoch`,
    ),
    sourceGeneration: positiveIntegerAt(
      source.source_generation,
      `${path}.source.source_generation`,
    ),
    assistantProjection,
    ownerIdentities,
    program,
    programBinding,
  };
}

function parseProgramApplyResult(
  value: unknown,
  path: string,
): Record<string, JsonValue> {
  const record = jsonRecordAt(value, path);
  exactKeysAt(
    record,
    [
      'applied',
      'preparation',
      'schema_version',
      'status',
    ],
    path,
  );
  if (
    stringAt(record.schema_version, `${path}.schema_version`)
    !== 'manyforge.program_apply_result.v2'
  ) {
    return fail(
      `${path}.schema_version`,
      'manyforge.program_apply_result.v2',
    );
  }
  const status = stringAt(record.status, `${path}.status`);
  if (status !== 'applied') {
    return fail(`${path}.status`, 'applied');
  }
  jsonRecordAt(record.preparation, `${path}.preparation`);
  parseAppliedProgramSelection(record.applied);
  return record;
}

function parseScenarioLoadResult(
  value: unknown,
  path: string,
): Record<string, JsonValue> {
  const record = jsonRecordAt(value, path);
  exactKeysAt(record, ['managerApply', 'programApply', 'scenario'], path);
  const scenario = jsonRecordAt(record.scenario, `${path}.scenario`);
  exactKeysAt(
    scenario,
    [
      'deployment_artifact_id',
      'deployment_revision',
      'graph_revision',
      'program_binding_id',
      'program_id',
      'program_revision',
      'retained_bundle_id',
      'source_document_digest',
      'scenario_id',
    ],
    `${path}.scenario`,
  );
  stringAt(scenario.deployment_artifact_id, `${path}.scenario.deployment_artifact_id`);
  stringAt(scenario.program_id, `${path}.scenario.program_id`);
  stringAt(scenario.scenario_id, `${path}.scenario.scenario_id`);
  for (const field of [
    'deployment_revision',
    'graph_revision',
    'program_binding_id',
    'program_revision',
    'retained_bundle_id',
    'source_document_digest',
  ]) {
    digestAt(scenario[field], `${path}.scenario.${field}`);
  }
  if (record.managerApply !== null) {
    jsonRecordAt(record.managerApply, `${path}.managerApply`);
  }
  if (record.programApply !== null) {
    parseProgramApplyResult(record.programApply, `${path}.programApply`);
  }
  return record;
}

export function parseProgramWorkspaceTransitionResponse(
  value: unknown,
): ProgramWorkspaceTransitionResponse {
  const path = 'programWorkspaceTransition';
  const record = objectAt(value, path);
  exactKeysAt(record, ['headless', 'status', 'workspace'], path);
  const status = stringAt(record.status, `${path}.status`);
  if (!['accepted', 'applied', 'loaded', 'reverted'].includes(status)) {
    return fail(
      `${path}.status`,
      'accepted, applied, loaded, or reverted',
    );
  }
  let headless: Record<string, JsonValue> | null;
  if (record.headless === null) {
    if (status !== 'reverted') {
      return fail(`${path}.headless`, 'an owner response');
    }
    headless = null;
  } else if (status === 'accepted' || status === 'applied') {
    const submitted = jsonRecordAt(record.headless, `${path}.headless`);
    exactKeysAt(submitted, [
      'status', 'confirmation_token', 'prepared_activation',
      'candidate_preview', 'manager_apply', 'preparation', 'phases_ms',
      'apply_id', ...(status === 'accepted' ? ['apply_generation'] : []),
    ], `${path}.headless`);
    if (submitted.status !== status || submitted.confirmation_token !== null
        || submitted.preparation !== null) {
      return fail(`${path}.headless`, 'the matching submitted activation');
    }
    parsePreparedActivationInput(submitted.prepared_activation,
      `${path}.headless.prepared_activation`);
    if (submitted.candidate_preview !== null) {
      parseManagerDeploymentCandidatePreviewResponse(submitted.candidate_preview);
    }
    objectAt(submitted.phases_ms, `${path}.headless.phases_ms`);
    if (status === 'accepted') {
      const manager = parseManagerApplyResponse(submitted.manager_apply);
      if (stringAt(submitted.apply_id, `${path}.headless.apply_id`)
          !== manager.applyId
          || positiveIntegerAt(submitted.apply_generation,
            `${path}.headless.apply_generation`) !== manager.applyGeneration) {
        return fail(`${path}.headless`, 'the exact accepted Manager operation');
      }
    } else if (submitted.manager_apply !== null || submitted.apply_id !== null
               || submitted.apply_generation !== undefined) {
      return fail(`${path}.headless`, 'an already-active configuration');
    }
    headless = submitted;
  } else {
    headless = status === 'loaded'
      ? parseScenarioLoadResult(record.headless, `${path}.headless`)
      : parseProgramApplyResult(record.headless, `${path}.headless`);
  }
  return {
    status: status as ProgramWorkspaceTransitionResponse['status'],
    headless,
    workspace: parseProgramWorkspaceResponse(record.workspace),
  };
}

export function parseAuthoringWorkspaceIdentity(
  value: unknown,
  path = 'authoringWorkspace',
): AuthoringWorkspaceIdentity {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'workspace_epoch', 'workspace_revision', 'program_draft_id',
    'program_draft_revision', 'deployment_draft_id', 'deployment_draft_revision',
  ], path);
  return {
    workspaceEpoch: stringAt(record.workspace_epoch, `${path}.workspace_epoch`),
    workspaceRevision: positiveIntegerAt(
      record.workspace_revision,
      `${path}.workspace_revision`,
    ),
    programDraftId: stringAt(record.program_draft_id, `${path}.program_draft_id`),
    programDraftRevision: integerAt(
      record.program_draft_revision,
      `${path}.program_draft_revision`,
    ),
    deploymentDraftId: stringAt(
      record.deployment_draft_id,
      `${path}.deployment_draft_id`,
    ),
    deploymentDraftRevision: integerAt(
      record.deployment_draft_revision,
      `${path}.deployment_draft_revision`,
    ),
  };
}

/** The published containment of the authoring workspace, or ``null``. */
export function parseAuthoringWorkspaceContainment(
  value: unknown,
  path = 'authoringWorkspaceContainment',
): AuthoringWorkspaceContainment | null {
  if (value === null) return null;
  const record = objectAt(value, path);
  exactKeysAt(record, ['cause', 'saved_path'], path);
  return {
    cause: stringAt(record.cause, `${path}.cause`),
    savedPath: nullableStringAt(record.saved_path, `${path}.saved_path`),
  };
}

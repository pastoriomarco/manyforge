import { readFileSync } from 'node:fs';
import { isValidElement, type ReactElement, type ReactNode } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { executionLocksObjectEditing, sceneSourceIsDraft } from '../state/viewerPresentationDraft';
import type { LogicalSceneObject } from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import { CopyObservedToDraftButton, DraftScenePreviewNotice } from './SceneAuthoringWorkspace';

const NOTICE = 'Showing Draft Scene preview, not the collision scene used for motion.';

type SceneSourceCase = Parameters<typeof sceneSourceIsDraft>;

/** The workspace shows the notice exactly while it presents the Draft Scene. */
function noticeMarkup(executionActive: boolean, ...source: SceneSourceCase): string {
  return renderToStaticMarkup(
    <DraftScenePreviewNotice
      shown={!executionActive && sceneSourceIsDraft(...source)}
      onShowRuntimeScene={() => undefined}
    />,
  );
}

function findButton(node: ReactNode): ReactElement<{ onClick: () => void; children: ReactNode }> | null {
  if (Array.isArray(node)) {
    for (const child of node) {
      const found = findButton(child);
      if (found !== null) return found;
    }
    return null;
  }
  if (!isValidElement<{ children?: ReactNode }>(node)) return null;
  if (node.type === 'button') {
    return node as ReactElement<{ onClick: () => void; children: ReactNode }>;
  }
  return findButton(node.props.children);
}

describe('Draft Scene preview notice', () => {
  it('is shown with the runtime Scene action while the Draft Scene has unapplied changes', () => {
    const markup = noticeMarkup(false, 'auto', true, true, true, true);
    expect(markup).toContain(NOTICE);
    expect(markup).toContain('role="status"');
    expect(markup).toContain('<button type="button">Show runtime Scene</button>');
  });

  it('is hidden whenever the viewport shows the runtime Scene', () => {
    // The Draft equals Applied: the runtime Scene is shown and no warning.
    expect(noticeMarkup(false, 'auto', true, true, true, false)).toBe('');
    // The operator chose the Applied (runtime) Scene view.
    expect(noticeMarkup(false, 'applied', true, true, true, true)).toBe('');
    // The Draft cannot be edited now, so only the live Scene is shown.
    expect(noticeMarkup(false, 'auto', true, true, false, true)).toBe('');
    // No Draft, or the runtime Scene is not yet known.
    expect(noticeMarkup(false, 'auto', true, false, true, true)).toBe('');
    expect(noticeMarkup(false, 'auto', false, true, true, true)).toBe('');
    // While execution runs the live Scene is shown, never the Draft.
    expect(noticeMarkup(true, 'auto', true, true, true, true)).toBe('');
  });

  it('switches the viewport to the runtime Scene from its action', () => {
    const onShowRuntimeScene = vi.fn();
    const button = findButton(
      DraftScenePreviewNotice({ shown: true, onShowRuntimeScene }),
    );
    expect(button?.props.children).toBe('Show runtime Scene');
    button?.props.onClick();
    expect(onShowRuntimeScene).toHaveBeenCalledTimes(1);
    expect(DraftScenePreviewNotice({ shown: false, onShowRuntimeScene })).toBeNull();
  });

  it('is driven by the presented Scene source and selects the runtime Scene', () => {
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );
    expect(workspace).toContain('const programDraftPreview = !executionActive && sceneSourceIsDraft(');
    expect(workspace).toMatch(
      /<DraftScenePreviewNotice\s+shown=\{programDraftPreview\}\s+onShowRuntimeScene=\{\(\) => setSceneSourceChoice\('applied'\)\}/,
    );
  });
});

describe('unavailable live Scene facts', () => {
  it('shows the owner-contract reason independently of the live object count', () => {
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );
    const objectPanelStart = workspace.indexOf('const liveObjectPanel = objects.length === 0 ? undefined');
    const viewerSurfaceStart = workspace.indexOf('const viewerSurface = (', objectPanelStart);
    const objectPanel = workspace.slice(objectPanelStart, viewerSurfaceStart);
    const editorSurfaceStart = workspace.indexOf('const editorSurface = (', viewerSurfaceStart);
    const editorSurfaceEnd = workspace.indexOf('\n  return (', editorSurfaceStart);
    const editorSurface = workspace.slice(editorSurfaceStart, editorSurfaceEnd);

    expect(objectPanelStart).toBeGreaterThanOrEqual(0);
    expect(objectPanel).not.toContain('factsResult.reason');
    expect(editorSurface).toContain('data-testid="scene-owner-facts-error"');
    expect(editorSurface).toContain('{factsResult.reason}');
    expect(editorSurface).toContain('!factsResult.available');
    expect(editorSurface).toContain('liveObjectPanel ?? (factsResult.available ? (');
  });
});

describe('current Scene detach action', () => {
  it('keeps the shared detach card on a selected carried Scene object', () => {
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );

    expect(workspace).toContain(
      "const selectedCarriedAttachment = selectedObject?.placement.kind === 'carried'",
    );
    expect(workspace).toMatch(
      /\{selectedObject\?\.placement\.kind === 'carried' && \(\s*<CarriedObjectPoseCard/,
    );
    expect(workspace).toContain('attachment={selectedCarriedAttachment}');
    expect(workspace).toContain('detachUnavailableReason={detachUnavailableReason}');
    expect(workspace).toContain('onDetach={detachCarriedObject}');
  });
});

describe('Program Object inspector detach wiring', () => {
  it('does not add the live detach action to the Program scene editor surface', () => {
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );

    expect(workspace).toContain('editable ? renderProgramSceneEditor(true) : liveObjectPanel');
    expect(workspace).toContain("{activeSceneTool === 'program' && renderProgramSceneEditor()}");
    expect(workspace).toContain('showRuntimeDetachAction={showRuntimeDetachAction}');
    expect(workspace).toContain('getDetachUnavailableReason={detachUnavailableReasonFor}');
    expect(workspace).toContain('onDetach={detachCarriedObject}');
    expect(workspace).toContain('const unavailable = detachUnavailableReasonFor(attachment);');
  });
});

describe('Copy current position to Draft', () => {
  const box: LogicalSceneObject['collision_shapes'][number] = {
    type: 'box', dimensions_m: { x: 0.1, y: 0.1, z: 0.1 },
  };
  const resting: LogicalSceneObject = {
    scene_resource_id: 'part',
    collision_shapes: [box],
    placement: {
      kind: 'cell_fixed',
      cell_id: 'cell',
      pose_in_cell: { position_m: { x: 0.4, y: 0.1, z: 0.2 }, orientation_xyzw: [0, 0, 0, 1] },
    },
    provider_evidence: null,
    support_contacts: [],
  };
  const carried: LogicalSceneObject = {
    ...resting,
    placement: {
      kind: 'carried',
      carried_object_attachment_id: 'attachment.1',
      carried_object_attachment_revision: '1',
    },
  };
  const button = (object: LogicalSceneObject | undefined, blockedReason: string | null = null) => {
    const mutate = vi.fn(async () => null);
    const element = CopyObservedToDraftButton({ object, blockedReason, mutate }) as ReactElement<{
      disabled: boolean;
      title: string;
      onClick: () => void;
    }>;
    return { ...element.props, mutate };
  };

  it('is enabled only for a copyable object while Draft editing is allowed', () => {
    const copy = button(resting);
    expect(copy.disabled).toBe(false);
    expect(copy.title).toContain('Copy this object as observed now in the live cell');
    copy.onClick();
    expect(copy.mutate).toHaveBeenCalledWith('program_draft_store_observed_scene_resource', {
      scene_resource_id: 'part',
    });
  });

  it('names why it is unavailable in its tooltip and sends nothing', () => {
    const editing = 'Program execution is running; editing requires Idle or all Program motion stopped.';
    const cases: Array<[LogicalSceneObject | undefined, string | null, string]> = [
      // The shared Draft-edit admission wins over every object condition.
      [resting, editing, editing],
      [carried, editing, editing],
      [carried, null, 'This object is Attached in the live cell; copy it once it is Detached.'],
      [{ ...resting, collision_shapes: [box, box] }, null,
        'Only a live object with exactly one collision shape can be copied.'],
      [undefined, null, 'This object is not in the live cell.'],
    ];
    for (const [object, blockedReason, reason] of cases) {
      const copy = button(object, blockedReason);
      expect(copy.disabled).toBe(true);
      expect(copy.title).toBe(reason);
      copy.onClick();
      expect(copy.mutate).not.toHaveBeenCalled();
    }
  });

  it('follows the same Draft-edit admission in the editor and the read-only live panel', () => {
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );
    expect(workspace).toContain('const draftEditBlocked = busyDraft || !programController.mutationAvailable;');
    expect(workspace).toContain("blockedReason={busy ? busyReason ?? 'The Program Draft cannot be edited now.' : null}");
    expect(workspace).toContain('blockedReason={draftEditBlocked ? draftEditBlockReason : null}');
  });
});

describe('viewer object panel during a Program run', () => {
  const cycle = (state: string, heldRunDraftEditAllowed: boolean) => ({
    programCycle: { state }, heldRunDraftEditAllowed,
  }) as unknown as ComposerHeadlessState;
  /**
   * The object panel's editability as the workspace derives it (wiring pinned
   * below), with the Draft itself editable: the Program workspace admits a
   * held-run edit through the same published fact.
   */
  const panelEditable = (snapshot: ComposerHeadlessState, draftDiffersFromApplied: boolean) => {
    const executionActive = executionLocksObjectEditing(snapshot);
    const draftSceneEditable = !executionActive
      && sceneSourceIsDraft('draft', true, true, true, draftDiffersFromApplied);
    const draftPreview = !executionActive
      && sceneSourceIsDraft('auto', true, true, true, draftDiffersFromApplied);
    return draftSceneEditable && (draftPreview || !draftDiffersFromApplied);
  };

  it('edits objects in a run whose Program motion is confirmed stopped', () => {
    for (const differs of [false, true]) {
      expect(panelEditable(cycle('running', true), differs)).toBe(true);
      expect(panelEditable(cycle('idle_after_reset', false), differs)).toBe(true);
    }
  });

  it('is read-only in a run whose motion is not confirmed stopped', () => {
    for (const differs of [false, true]) {
      expect(panelEditable(cycle('running', false), differs)).toBe(false);
    }
  });

  it('derives the object panel from the published held-run fact', () => {
    const workspace = readFileSync(
      new URL('./SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );
    expect(workspace).toContain('const executionActive = executionLocksObjectEditing(headlessState.snapshot);');
    expect(workspace).toContain('const draftSceneEditable = !executionActive && sceneSourceIsDraft(');
    expect(workspace).toMatch(/const objectsEditable = draftSceneEditable\s+&& \(programDraftPreview \|\| !draftDiffersFromApplied\);/);
    expect(workspace).toContain('objectControlSurface={objectControlSurface(objectsEditable)}');
  });
});

describe('Scene revision changes in the authoring workspace', () => {
  const workspace = () => readFileSync(new URL('./SceneAuthoringWorkspace.tsx', import.meta.url), 'utf8');

  it('keeps exact drag identity separate from tolerant measured-pose stability', () => {
    const source = workspace();
    const start = source.indexOf('identity: JSON.stringify([observationEpoch');
    const end = source.indexOf(']),', start);
    const identity = source.slice(start, end);
    const fields = [
      'observationEpoch',
      'status.retainedBundleId',
      'status.graphRevision',
      'motion.programBindingId',
      'base',
      'tcp',
    ];
    const fieldOffsets = fields.map((field) => identity.indexOf(field));

    expect(start).toBeGreaterThan(0);
    expect(fieldOffsets.every((offset) => offset >= 0)).toBe(true);
    expect(fieldOffsets).toEqual([...fieldOffsets].sort((left, right) => left - right));
    expect(identity).not.toMatch(/basePose|tcpPose/);
    expect(source).toContain('candidateDragReferenceIsStable(dragReference.current, reference)');
    // The TCP target still uses the release-time measured base transform.
    expect(source).toContain('pose, reference.base)');
  });

  it('keys the candidate-TCP gizmo without the Scene, so pose updates never cancel a drag', () => {
    const source = workspace();
    const start = source.indexOf('candidateContextKey={JSON.stringify([');
    const key = source.slice(start, source.indexOf('])}', start));
    expect(start).toBeGreaterThan(0);
    expect(key).not.toMatch(/scene|revision_id|content_hash/i);
  });

  it('keys runtime pose-source rows by source identity, not by observation sequence', () => {
    const source = workspace();
    const start = source.indexOf('className="runtime-source-row"');
    const row = source.slice(start, source.indexOf('>', start));
    expect(row).toContain('key={`${source.object_id}:${source.policy_id}:${source.source_epoch}:${source.source_id}`}');
    expect(row).not.toContain('source_sequence');
  });
});

import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { sceneDetachUnavailableReason } from '../state/sceneDetach';
import type {
  CarriedObjectAttachment,
  LogicalSceneObject,
  ProgramAuthoringDocument,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { SceneOwnerFacts } from '../state/sceneAuthoring';

const sceneCommit = vi.hoisted(() => vi.fn());
vi.mock('../api/client', () => ({
  api: { scene: { commit: sceneCommit }, files: { browse: vi.fn() } },
}));

import { ProgramSceneEditor } from './SceneAuthoringWorkspace';

afterEach(() => {
  vi.unstubAllGlobals();
});

type FakeNode = Record<string, unknown> & {
  children: FakeNode[];
  parentNode: FakeNode | null;
};
type ReactProps = Record<string, unknown> & {
  onChange?: (event: unknown) => void;
  onBlurCapture?: (event: unknown) => void;
  onClick?: () => unknown;
  children?: unknown;
  disabled?: boolean;
  role?: string;
  'aria-modal'?: boolean;
};

// The minimal React DOM host of FrameSelector.mounted.test.tsx: real commits,
// state and effects; every created element is kept so the test calls the
// React handlers of one mounted control.
function stubDom() {
  const created: FakeNode[] = [];
  const node = (tag: string): FakeNode => {
    const element: FakeNode = {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {},
      options: [], children: [], parentNode: null,
      addEventListener: () => undefined, removeEventListener: () => undefined,
      appendChild: (child: FakeNode) => {
        element.children.push(child);
        child.parentNode = element;
        return child;
      },
      insertBefore: (child: FakeNode, before: FakeNode) => {
        const index = element.children.indexOf(before);
        element.children.splice(index < 0 ? element.children.length : index, 0, child);
        child.parentNode = element;
        return child;
      },
      removeChild: (child: FakeNode) => {
        const index = element.children.indexOf(child);
        if (index >= 0) element.children.splice(index, 1);
        child.parentNode = null;
        return child;
      },
      setAttribute: () => undefined, removeAttribute: () => undefined,
      focus: () => { document.activeElement = element; },
    };
    created.push(element);
    return element;
  };
  const document: Record<string, unknown> = {
    nodeType: 9, activeElement: null, defaultView: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    createTextNode: (text: string) => ({
      ...node('text'), nodeType: 3, nodeValue: text,
    }),
  };
  document.createElement = node;
  document.documentElement = node('html');
  const window = {
    document, HTMLIFrameElement: class {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout,
    requestAnimationFrame: (callback: () => void) => { callback(); return 0; },
  };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  return { host: node('div'), created };
}

function reactProps(element: FakeNode): ReactProps | undefined {
  const key = Object.keys(element).find((name) => name.startsWith('__reactProps$'));
  return key === undefined ? undefined : element[key] as ReactProps;
}

function mountedProps(host: FakeNode): ReactProps[] {
  const walk = (element: FakeNode): FakeNode[] => [
    element,
    ...element.children.flatMap(walk),
  ];
  return walk(host).map(reactProps).filter((props): props is ReactProps => props !== undefined);
}

function mountedButton(
  host: FakeNode,
  label: string,
  last = false,
): ReactProps {
  const buttons = mountedProps(host).filter((props) => (
    props.children === label && typeof props.onClick === 'function'
  ));
  const button = last ? buttons[buttons.length - 1] : buttons[0];
  if (button === undefined) throw new Error(`Mounted ${label} button not found`);
  return button;
}

async function clickMountedButton(
  host: FakeNode,
  label: string,
  last = false,
): Promise<void> {
  const button = mountedButton(host, label, last);
  await act(async () => { await button.onClick?.(); });
}

const program = {
  component_roles: {},
  target_roles: {},
  scene: [{
    scene_resource_id: 'part',
    shape: { type: 'box', box_dimensions_m: [0.1, 0.1, 0.1] },
    pose: {
      frame_ref: { kind: 'cell_frame' },
      position_m: [0.4, 0.1, 0.2],
      orientation: { xyzw: [0, 0, 0, 1] },
    },
  }],
} as unknown as ProgramAuthoringDocument;

const currentAttachment: CarriedObjectAttachment = {
  carried_object_attachment_id: 'attachment.1',
  carried_object_attachment_revision: 'attachment.revision.1',
  scene_resource_id: 'part',
  controllable_component_id: 'arm',
  command_target_id: 'gripper',
  carrier_frame_ref: {
    kind: 'component', controllable_component_id: 'arm',
    model_instance_id: 'arm.model', frame_role_id: 'flange',
  },
  pose_in_carrier_frame: {
    position_m: { x: 0, y: 0, z: 0 }, orientation_xyzw: [0, 0, 0, 1],
  },
  touch_body_roles: [],
};

const currentCarriedObject: LogicalSceneObject = {
  scene_resource_id: 'part',
  collision_shapes: [],
  placement: {
    kind: 'carried',
    carried_object_attachment_id: currentAttachment.carried_object_attachment_id,
    carried_object_attachment_revision: currentAttachment.carried_object_attachment_revision,
  },
  provider_evidence: null,
  support_contacts: [],
};

const activeProjection = {
  retained_bundle_id: 'bundle.1',
  graph_revision: 'graph.1',
  scene_revision: { scene_id: 'scene.1', revision_id: 7, content_hash: 'scene.hash.7' },
};

function sceneFactsWithAttachment(attachment: CarriedObjectAttachment): SceneOwnerFacts {
  return {
    sourceOwnerEpoch: 'scene.owner.1',
    logicalScene: {
      access: {
        active_projection: activeProjection,
        scene_writes_open: true,
        projection_leases_open: true,
      },
      snapshot: { carried_object_attachments: [attachment] },
    },
  } as unknown as SceneOwnerFacts;
}

const heldRunSnapshot = {
  programCycle: {
    state: 'running',
    programRunId: 'run.1',
    runtimeControls: { executionSelections: { motionHeld: true } },
  },
} as unknown as ComposerHeadlessState;

const currentControl = {
  transportStatus: 'connected' as const,
  semanticStatus: 'current' as const,
  transportError: null,
};

function carriedObjectFor(attachment: CarriedObjectAttachment): LogicalSceneObject {
  return {
    ...currentCarriedObject,
    placement: {
      kind: 'carried',
      carried_object_attachment_id: attachment.carried_object_attachment_id,
      carried_object_attachment_revision: attachment.carried_object_attachment_revision,
    },
  };
}

function programObjectInspectorElement(
  liveObjects: LogicalSceneObject[],
  runtimeCarriedAttachments: CarriedObjectAttachment[],
  onDetach: (attachment: CarriedObjectAttachment) => Promise<void>,
  getDetachUnavailableReason: (attachment: CarriedObjectAttachment) => string | null,
) {
  return (
    <ProgramSceneEditor
      program={program}
      draftRevision={3}
      busy={false}
      busyReason={null}
      mutate={vi.fn(async () => null)}
      liveObjects={liveObjects}
      selectedResourceId="part"
      onSelectResource={() => undefined}
      runtimeCarriedAttachments={runtimeCarriedAttachments}
      getDetachUnavailableReason={getDetachUnavailableReason}
      onDetach={onDetach}
      showRuntimeDetachAction
    />
  );
}

function programSceneMarkup(
  liveObjects: LogicalSceneObject[],
  runtimeCarriedAttachments: CarriedObjectAttachment[],
  showRuntimeDetachAction = true,
) {
  return renderToStaticMarkup(
    <ProgramSceneEditor
      program={program}
      draftRevision={3}
      busy={false}
      busyReason={null}
      mutate={vi.fn(async () => null)}
      liveObjects={liveObjects}
      selectedResourceId="part"
      onSelectResource={() => undefined}
      runtimeCarriedAttachments={runtimeCarriedAttachments}
      getDetachUnavailableReason={() => null}
      onDetach={async () => {}}
      showRuntimeDetachAction={showRuntimeDetachAction}
    />,
  );
}

describe('Program Object inspector detach action', () => {
  it('shows the shared Detach action only in the Object inspector for a matching live carried placement', () => {
    const editorMarkup = programSceneMarkup(
      [currentCarriedObject], [currentAttachment], false,
    );
    const inspectorMarkup = programSceneMarkup(
      [currentCarriedObject], [currentAttachment], true,
    );

    expect(editorMarkup).not.toContain('aria-label="Carried object pose"');
    expect(inspectorMarkup).toContain('aria-label="Carried object pose"');
    expect(inspectorMarkup).toMatch(/class="danger-button">Detach<\/button>/);
  });

  it('hides detach for unattached and mismatched runtime placements', () => {
    const fixedObject: LogicalSceneObject = {
      ...currentCarriedObject,
      placement: {
        kind: 'cell_fixed',
        cell_id: 'cell',
        pose_in_cell: {
          position_m: { x: 0, y: 0, z: 0 },
          orientation_xyzw: [0, 0, 0, 1],
        },
      },
    };
    const mismatchedAttachment = {
      ...currentAttachment,
      scene_resource_id: 'other',
    };
    const mismatchedAttachmentId = {
      ...currentAttachment,
      carried_object_attachment_id: 'attachment.other',
    };
    const mismatchedAttachmentRevision = {
      ...currentAttachment,
      carried_object_attachment_revision: 'attachment.revision.other',
    };
    const otherCarriedObject: LogicalSceneObject = {
      ...currentCarriedObject,
      scene_resource_id: 'other',
    };

    for (const markup of [
      programSceneMarkup([fixedObject], [currentAttachment]),
      programSceneMarkup([currentCarriedObject], [mismatchedAttachment]),
      programSceneMarkup([currentCarriedObject], [mismatchedAttachmentId]),
      programSceneMarkup([currentCarriedObject], [mismatchedAttachmentRevision]),
      programSceneMarkup([otherCarriedObject], [currentAttachment]),
    ]) {
      expect(markup).not.toContain('aria-label="Carried object pose"');
      expect(markup).not.toMatch(/class="danger-button">Detach<\/button>/);
    }
  });
});

describe('mounted Program Object inspector detach action', () => {
  it('mounts the shared action only when attachment ID and revision match the live placement', async () => {
    const { host } = stubDom();
    const onDetach = vi.fn(async (_attachment: CarriedObjectAttachment) => {});
    const getReason = (attachment: CarriedObjectAttachment) => (
      sceneDetachUnavailableReason(
        heldRunSnapshot,
        sceneFactsWithAttachment(attachment),
        attachment,
        currentControl,
      )
    );
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    const mismatches: CarriedObjectAttachment[] = [
      {
        ...currentAttachment,
        carried_object_attachment_id: 'attachment.other',
      },
      {
        ...currentAttachment,
        carried_object_attachment_revision: 'attachment.revision.other',
      },
    ];
    try {
      for (const attachment of mismatches) {
        await act(() => root.render(programObjectInspectorElement(
          [currentCarriedObject], [attachment], onDetach, getReason,
        )));
        expect(mountedProps(host).some((props) => (
          props['aria-label'] === 'Carried object pose'
        ))).toBe(false);
        expect(mountedProps(host).some((props) => (
          props.children === 'Detach' && typeof props.onClick === 'function'
        ))).toBe(false);
      }

      await act(() => root.render(programObjectInspectorElement(
        [currentCarriedObject], [currentAttachment], onDetach, getReason,
      )));
      expect(mountedProps(host).some((props) => (
        props['aria-label'] === 'Carried object pose'
      ))).toBe(true);
      expect(mountedButton(host, 'Detach').disabled).toBe(false);
    } finally {
      await act(() => root.unmount());
    }
  });

  it('lets a held run confirm detach and submits the latest revision after a routine attachment update', async () => {
    const { host } = stubDom();
    const onDetach = vi.fn(async (_attachment: CarriedObjectAttachment) => {});
    let sceneAttachment = currentAttachment;
    const getReason = (attachment: CarriedObjectAttachment) => (
      sceneDetachUnavailableReason(
        heldRunSnapshot,
        sceneFactsWithAttachment(sceneAttachment),
        attachment,
        currentControl,
      )
    );
    const renderInspector = (
      liveObject: LogicalSceneObject,
      attachment: CarriedObjectAttachment,
    ) => programObjectInspectorElement([liveObject], [attachment], onDetach, getReason);
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(renderInspector(currentCarriedObject, currentAttachment)));
    try {
      expect(getReason(currentAttachment)).toBeNull();
      expect(mountedButton(host, 'Detach').disabled).toBe(false);
      await clickMountedButton(host, 'Detach');
      expect(mountedProps(host).some((props) => (
        props.role === 'dialog' && props['aria-modal'] === true
      ))).toBe(true);
      expect(mountedProps(host).some((props) => (
        props.children === 'Detach part from arm / gripper at its current pose?'
      ))).toBe(true);

      // DX3323 keeps confirmation identity bound to the part and holder, not
      // the internal attachment revision. The backend still receives the
      // latest revision and fences the detach against that exact value.
      const revisedAttachment: CarriedObjectAttachment = {
        ...currentAttachment,
        carried_object_attachment_revision: 'attachment.revision.2',
        pose_in_carrier_frame: {
          ...currentAttachment.pose_in_carrier_frame,
          position_m: { x: 0.01, y: 0, z: 0 },
        },
      };
      sceneAttachment = revisedAttachment;
      await act(() => root.render(renderInspector(
        carriedObjectFor(revisedAttachment), revisedAttachment,
      )));
      expect(mountedProps(host).some((props) => (
        props.children === 'The carried-object attachment changed. Review the confirmation again.'
      ))).toBe(false);
      expect(mountedButton(host, 'Detach', true).disabled).toBe(false);
      expect(onDetach).not.toHaveBeenCalled();

      await clickMountedButton(host, 'Detach', true);
      expect(onDetach).toHaveBeenCalledExactlyOnceWith(revisedAttachment);
    } finally {
      await act(() => root.unmount());
    }
  });

  it('requires a fresh confirmation when the attachment ID changes', async () => {
    const { host } = stubDom();
    const onDetach = vi.fn(async (_attachment: CarriedObjectAttachment) => {});
    let sceneAttachment = currentAttachment;
    const getReason = (attachment: CarriedObjectAttachment) => (
      sceneDetachUnavailableReason(
        heldRunSnapshot,
        sceneFactsWithAttachment(sceneAttachment),
        attachment,
        currentControl,
      )
    );
    const renderInspector = (
      liveObject: LogicalSceneObject,
      attachment: CarriedObjectAttachment,
    ) => programObjectInspectorElement([liveObject], [attachment], onDetach, getReason);
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(renderInspector(currentCarriedObject, currentAttachment)));
    try {
      await clickMountedButton(host, 'Detach');
      const changedAttachment: CarriedObjectAttachment = {
        ...currentAttachment,
        carried_object_attachment_id: 'attachment.2',
        carried_object_attachment_revision: 'attachment.revision.2',
      };
      sceneAttachment = changedAttachment;
      await act(() => root.render(renderInspector(
        carriedObjectFor(changedAttachment), changedAttachment,
      )));

      expect(mountedProps(host).some((props) => (
        props.children === 'The carried-object attachment changed. Review the confirmation again.'
      ))).toBe(true);
      expect(mountedButton(host, 'Detach', true).disabled).toBe(true);
      expect(onDetach).not.toHaveBeenCalled();

      // Reopening the trigger records the new attachment identity in the
      // existing shared confirmation; only then can the user submit it.
      await clickMountedButton(host, 'Detach');
      expect(mountedProps(host).some((props) => (
        props.children === 'The carried-object attachment changed. Review the confirmation again.'
      ))).toBe(false);
      expect(mountedButton(host, 'Detach', true).disabled).toBe(false);
      await clickMountedButton(host, 'Detach', true);
      expect(onDetach).toHaveBeenCalledExactlyOnceWith(changedAttachment);
    } finally {
      await act(() => root.unmount());
    }
  });

  it('disables the held-run action when transport or semantic sources are stale', async () => {
    const { host } = stubDom();
    const onDetach = vi.fn(async (_attachment: CarriedObjectAttachment) => {});
    const staleControls = [
      {
        ...currentControl,
        transportStatus: 'stale' as const,
      },
      {
        ...currentControl,
        semanticStatus: 'synchronizing' as const,
      },
    ];
    const expectedReasons = [
      'Program motion transport is not connected; wait for current state before detaching.',
      'Program motion state is not semantically current; wait before detaching.',
    ];
    const renderInspector = (control: typeof staleControls[number]) => (
      programObjectInspectorElement(
        [currentCarriedObject],
        [currentAttachment],
        onDetach,
        (attachment) => sceneDetachUnavailableReason(
          heldRunSnapshot,
          sceneFactsWithAttachment(currentAttachment),
          attachment,
          control,
        ),
      )
    );
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(renderInspector(staleControls[0])));
    try {
      for (const [index, control] of staleControls.entries()) {
        if (index > 0) await act(() => root.render(renderInspector(control)));
        const trigger = mountedButton(host, 'Detach');
        expect(trigger.disabled).toBe(true);
        expect(trigger.title).toBe(expectedReasons[index]);
        await clickMountedButton(host, 'Detach');
        expect(mountedProps(host).some((props) => props.role === 'dialog')).toBe(false);
        expect(onDetach).not.toHaveBeenCalled();
      }
    } finally {
      await act(() => root.unmount());
    }
  });
});

describe('viewer object panel', () => {
  it('commits a pose field change as one Draft update and no live Scene transaction', async () => {
    const { host, created } = stubDom();
    const mutate = vi.fn(async () => null);
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => {
      root.render(
        <ProgramSceneEditor
          program={program}
          draftRevision={3}
          busy={false}
          busyReason={null}
          mutate={mutate}
          liveObjects={[]}
          selectedResourceId="part"
          onSelectResource={() => undefined}
        />,
      );
    });
    const mounted = (predicate: (props: ReactProps) => boolean) => {
      const element = created.find((candidate) => {
        const props = reactProps(candidate);
        return props !== undefined && predicate(props);
      });
      if (element === undefined) throw new Error('control is not mounted');
      return reactProps(element)!;
    };
    const x = mounted((props) => props['aria-label'] === 'Draft X position (m)');
    await act(() => {
      x.onChange!({ currentTarget: { valueAsNumber: 0.5 } });
    });
    // Editing the field alone sends nothing; committing it (blur) is one edit.
    expect(mutate).not.toHaveBeenCalled();
    const editor = mounted((props) => props['data-testid'] === 'program-scene-editor');
    await act(() => {
      editor.onBlurCapture!({ target: { tagName: 'INPUT' } });
    });

    expect(mutate).toHaveBeenCalledTimes(1);
    expect(mutate).toHaveBeenCalledWith('scene_draft_update_object', {
      scene_resource: expect.objectContaining({
        scene_resource_id: 'part',
        pose: expect.objectContaining({ position_m: [0.5, 0.1, 0.2] }),
      }),
    });
    expect(sceneCommit).not.toHaveBeenCalled();
    await act(() => { root.unmount(); });
  });
});

describe('object mass field', () => {
  const withMetadata = (metadata?: Record<string, unknown>) => ({
    ...program,
    scene: [{ ...program.scene![0], ...(metadata === undefined ? {} : { metadata }) }],
  } as unknown as ProgramAuthoringDocument);

  async function editMass(document: ProgramAuthoringDocument, text: string) {
    const { host, created } = stubDom();
    const mutate = vi.fn(async () => null);
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => {
      root.render(
        <ProgramSceneEditor
          program={document}
          draftRevision={3}
          busy={false}
          busyReason={null}
          mutate={mutate}
          liveObjects={[]}
          selectedResourceId="part"
          onSelectResource={() => undefined}
        />,
      );
    });
    const mounted = (predicate: (props: ReactProps) => boolean) => {
      const element = created.find((candidate) => {
        const props = reactProps(candidate);
        return props !== undefined && predicate(props);
      });
      return element === undefined ? undefined : reactProps(element)!;
    };
    const field = mounted((props) => props['aria-label'] === 'Draft object mass (kg)')!;
    await act(() => {
      field.onChange!({ currentTarget: { value: text } });
    });
    await act(() => {
      mounted((props) => props['data-testid'] === 'program-scene-editor')!
        .onBlurCapture!({ target: { tagName: 'INPUT' } });
    });
    const alert = mounted((props) => props.role === 'alert')?.children;
    await act(() => { root.unmount(); });
    return { mutate, alert };
  }

  it('writes metadata.mass_kg as one Draft edit and keeps the other metadata', async () => {
    const { mutate, alert } = await editMass(withMetadata({ note: 'fixture' }), '2.5');

    expect(alert).toBeUndefined();
    expect(mutate).toHaveBeenCalledTimes(1);
    expect(mutate).toHaveBeenCalledWith('scene_draft_update_object', {
      scene_resource: expect.objectContaining({
        scene_resource_id: 'part',
        metadata: { note: 'fixture', mass_kg: 2.5 },
        pose: expect.objectContaining({ position_m: [0.4, 0.1, 0.2] }),
      }),
    });
    expect(sceneCommit).not.toHaveBeenCalled();
  });

  it('removes the key when the field is emptied (0 kg), and the metadata when nothing is left', async () => {
    const kept = await editMass(withMetadata({ note: 'fixture', mass_kg: 2 }), '');
    expect(kept.mutate).toHaveBeenCalledExactlyOnceWith('scene_draft_update_object', {
      scene_resource: expect.objectContaining({ metadata: { note: 'fixture' } }),
    });

    const emptied = await editMass(withMetadata({ mass_kg: 2 }), '');
    expect(emptied.mutate).toHaveBeenCalledTimes(1);
    const [, payload] = (emptied.mutate.mock.calls[0] as unknown) as [string, { scene_resource: object }];
    expect(payload.scene_resource).not.toHaveProperty('metadata');
  });

  it('commits an explicit 0 as no key, and refuses a negative or over-bound mass without a Draft edit', async () => {
    // One representation of "no mass": 0 removes the key, like an empty field.
    const zero = await editMass(withMetadata({ note: 'fixture', mass_kg: 2 }), '0');
    expect(zero.mutate).toHaveBeenCalledExactlyOnceWith('scene_draft_update_object', {
      scene_resource: expect.objectContaining({ metadata: { note: 'fixture' } }),
    });
    const zeroOnly = await editMass(withMetadata({ mass_kg: 2 }), '0');
    const [, payload] = (zeroOnly.mutate.mock.calls[0] as unknown) as [string, { scene_resource: object }];
    expect(payload.scene_resource).not.toHaveProperty('metadata');
    // 0 on a massless object is no change: no Draft edit.
    expect((await editMass(withMetadata(), '0')).mutate).not.toHaveBeenCalled();

    for (const text of ['-1', '1000.5']) {
      const { mutate, alert } = await editMass(withMetadata(), text);
      expect(mutate).not.toHaveBeenCalled();
      expect(alert).toBe('Mass must be between 0 and 1000 kg.');
    }
  });
});

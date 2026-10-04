import { readFileSync } from 'node:fs';
import { isValidElement, type ReactElement, type ReactNode } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { sceneSourceIsDraft } from '../state/viewerPresentationDraft';
import { DraftScenePreviewNotice } from './SceneAuthoringWorkspace';

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

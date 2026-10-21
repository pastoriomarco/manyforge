/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import {
  KernelErrorPanel,
  errorPresentationKey,
  prettyBodyId,
  prettyPair,
  readableErrorCode,
} from './KernelErrorPanel';

describe('Kernel diagnostic presentation shell', () => {
  it('keeps identity stable across detail-object key order', () => {
    expect(errorPresentationKey({
      code: 'route_failed', message: 'No route', details: { b: 2, a: 1 },
    })).toBe(errorPresentationKey({
      code: 'route_failed', message: 'No route', details: { a: 1, b: 2 },
    }));
  });

  it('keeps one owner-issued incident stable across incremental diagnostics', () => {
    const first = errorPresentationKey({
      code: 'runtime_collision',
      message: 'Motion interrupted',
      details: {
        runtime_fault_id: 'runtime-fault-7',
        execution_id: 42,
        lookahead_checked_samples: 8,
      },
    });
    const updated = errorPresentationKey({
      code: 'runtime_collision',
      message: 'Motion interrupted',
      details: {
        runtime_fault_id: 'runtime-fault-7',
        execution_id: 42,
        lookahead_checked_samples: 32,
      },
    });
    const next = errorPresentationKey({
      code: 'runtime_collision',
      message: 'Motion interrupted',
      details: { runtime_fault_id: 'runtime-fault-8', execution_id: 43 },
    });

    expect(updated).toBe(first);
    expect(next).not.toBe(first);
  });

  it('retains owner-neutral body and pair labels', () => {
    expect(prettyPair(
      'object/wall/shape/0 <-> robot/robot0/link/left_outer_finger/collision/0',
    )).toBe('wall ↔ left_outer_finger');
    expect(prettyPair(
      'object/ground/shape/0 <-> robot/robot0/attached/graspable/shape/0',
    )).toBe('ground ↔ attached graspable');
    expect(prettyPair('object/wall/shape/0')).toBe('object/wall/shape/0');
    expect(prettyBodyId('object/isaac_cheese_box/shape/0')).toBe(
      'isaac_cheese_box',
    );
    expect(prettyBodyId('custom/body/token')).toBe('token');
  });

  it('uses readable fallback copy without inventing code-specific policy', () => {
    expect(readableErrorCode('route_failed')).toBe('route failed');
    const html = renderToStaticMarkup(createElement(KernelErrorPanel, {
      error: {
        code: 'route_failed',
        message: 'No qualified route is available.',
        details: { component_id: 'robot-1' },
      },
    }));
    expect(html).toContain('role="alert"');
    expect(html).toContain('No qualified route is available.');
    expect(html).toContain('component id');
  });

  it('renders presentation supplied by the future exact projection adapter', () => {
    const html = renderToStaticMarkup(createElement(KernelErrorPanel, {
      error: { code: 'qualified_code', message: 'raw' },
      presentation: {
        headline: 'Motion route unavailable',
        hint: 'Review the selected component target.',
        severity: 'critical',
      },
      presentationKey: 'owner-issued-incident-7',
    }));
    expect(html).toContain('Motion route unavailable');
    expect(html).toContain('Review the selected component target.');
    expect(html).toContain('severity-critical');
    expect(html).toContain('data-presentation-key="owner-issued-incident-7"');
  });

  it('puts the status line under the cause and the code and details behind Technical details', () => {
    const html = renderToStaticMarkup(createElement(KernelErrorPanel, {
      error: { code: 'qualified_code', message: 'raw', details: { node: 'n1' } },
      presentation: {
        headline: 'Pose goal unreachable',
        status: 'Program operation failed · Active',
        hint: '',
        severity: 'error',
      },
    }));
    expect(html).toContain('<h2>Pose goal unreachable</h2><p class="kernel-error-status">Program operation failed · Active</p>');
    expect(html).not.toContain('<p>raw</p>');
    expect(html).toContain('<details class="kernel-error-technical"><summary>Technical details</summary><code>qualified_code</code><dl>');
  });

  it('renders the retained dismiss control only when a callback is supplied', () => {
    const error = { code: 'route_failed', message: 'No route' };
    expect(renderToStaticMarkup(createElement(KernelErrorPanel, { error })))
      .not.toContain('>Dismiss</button>');
    expect(renderToStaticMarkup(createElement(KernelErrorPanel, {
      error,
      onDismiss: () => undefined,
    }))).toContain('>Dismiss</button>');
  });
});

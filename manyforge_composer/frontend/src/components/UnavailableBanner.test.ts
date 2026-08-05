/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { UnavailableBanner } from './UnavailableBanner';

describe('headless availability banner', () => {
  it('does not call a configured but unreachable headless API connected', () => {
    const html = renderToStaticMarkup(createElement(UnavailableBanner, {
      health: {
        status: 'ready',
        data: {
          status: 'degraded',
          mode: 'headless_client',
          components: { headless_projection: 'unavailable' },
          runtimeAvailability: {
            code: 'headless_http_unavailable',
            message: 'Connection refused.',
            restorationGates: [],
          },
          headless: {
            status_scope: 'headless_projection',
            status: 'unavailable',
            headless_projection_ready: false,
            robot_readiness: { available: false },
          },
          timestamp: 1,
        },
      },
      onRetry: () => undefined,
    }));

    expect(html).toContain('Runtime operations are unavailable');
    expect(html).not.toContain('Headless runtime truth is connected');
    expect(html).toContain('Check headless API connectivity and configuration');
  });

  it('describes later gates without claiming robot readiness when H0 responds', () => {
    const html = renderToStaticMarkup(createElement(UnavailableBanner, {
      health: {
        status: 'ready',
        data: {
          status: 'degraded',
          mode: 'headless_client',
          components: { headless_projection: 'degraded' },
          runtimeAvailability: null,
          headless: {
            status_scope: 'headless_projection',
            status: 'degraded',
            headless_projection_ready: false,
            robot_readiness: { available: false },
          },
          timestamp: 1,
        },
      },
      onRetry: () => undefined,
    }));

    expect(html).toContain('Headless runtime truth is connected');
    expect(html).toContain('Program execution and motion remain unavailable');
    expect(html).toContain('Later capabilities restore at N');
  });
});

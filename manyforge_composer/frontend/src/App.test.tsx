import { readFileSync } from 'node:fs';

import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import App from './App';
import type { HealthResponse } from './types/api';
import { readStyleSource } from './styles/readStyleSource';

const healthProjection = vi.hoisted(() => ({ value: null as HealthResponse | null }));

vi.mock('./state/structuralResource', async (importOriginal) => {
  const actual = await importOriginal<typeof import('./state/structuralResource')>();
  const { api } = await import('./api/client');
  return {
    ...actual,
    useStructuralResource: (...args: Parameters<typeof actual.useStructuralResource>) => {
      const resource = actual.useStructuralResource(...args);
      return args[0] === api.health.get && healthProjection.value !== null
        ? [{ status: 'ready', data: healthProjection.value }, () => 0]
        : resource;
    },
  };
});

afterEach(() => { healthProjection.value = null; });

describe('Composer shell continuity', () => {
  it('shows only the backend-projected session notice in the existing warning rail', async () => {
    const health: HealthResponse = {
      status: 'ok', mode: 'headless_client', components: {},
      runtimeAvailability: null, headless: null, timestamp: 1,
    };
    healthProjection.value = health;
    expect(renderToStaticMarkup(<App />)).not.toContain('ManyForge did not close correctly last time.');
    // Resolve the existing lazy viewport so its real runtime overlay is rendered.
    await import('./components/StructuralViewer');
    await Promise.resolve();
    healthProjection.value = { ...health, workbenchSessionNotice: {
      schemaVersion: 'manyforge.workbench_previous_session.v1',
      sessionId: 'a'.repeat(32),
      archiveRelativePath: `previous-sessions/${'a'.repeat(32)}`,
      shutdown: 'clean', effects: 'unknown', historyCoverage: 'not_imported',
      journalFamilies: ['program_cycle', 'scene', 'manager', 'cclc', 'kernel_operation'],
      message: 'Session a: journals retained; motion effects unknown. Nothing was resumed.',
    } };
    const markup = renderToStaticMarkup(<App />);
    expect(markup).toContain('ManyForge did not close correctly last time.');
    expect(markup).toContain('The cell started empty; nothing from the previous session was resumed. Its records are kept; details in History.');
    expect(markup).toContain('workbench-notice-row severity-warning');
    const noticeHealth = healthProjection.value;
    for (const status of ['registered', 'acknowledged', 'quiet']) {
      healthProjection.value = { ...noticeHealth, headless: {
        workbench_previous_session_registration: { session_id: 'a'.repeat(32), status },
      } };
      expect(renderToStaticMarkup(<App />)).not.toContain('ManyForge did not close correctly last time.');
    }
    healthProjection.value = { ...noticeHealth, headless: {
      workbench_previous_session_registration: { session_id: 'b'.repeat(32), status: 'registered' },
    } };
    expect(renderToStaticMarkup(<App />)).toContain('ManyForge did not close correctly last time.');
    healthProjection.value = { ...health, workbenchSessionNotice: null };
    expect(renderToStaticMarkup(<App />)).not.toContain('ManyForge did not close correctly last time.');
  });

  it('shows a refused startup project in the warning rail until it is cleared', () => {
    const health: HealthResponse = {
      status: 'ok', mode: 'headless_client', components: {},
      runtimeAvailability: null, headless: null, timestamp: 1,
    };
    const message = 'Startup project /p/cell.scenario was not opened: a Scenario is still applied. Close it, then Load.';
    healthProjection.value = { ...health, startupScenarioNotice: {
      path: '/p/cell.scenario', code: 'scenario_close_required', message,
    } };
    const markup = renderToStaticMarkup(<App />);
    expect(markup).toContain('Startup project was not opened.');
    expect(markup).toContain(message);
    expect(markup).toContain('workbench-notice-row severity-warning');
    // The refreshed health after a successful Load no longer has it.
    healthProjection.value = { ...health, startupScenarioNotice: null };
    expect(renderToStaticMarkup(<App />)).not.toContain('Startup project was not opened.');
    // A restarted backend reports a new refusal: nothing earlier hides it.
    healthProjection.value = { ...health, startupScenarioNotice: {
      path: '/p/cell.scenario', code: 'scenario_close_required', message,
    } };
    expect(renderToStaticMarkup(<App />)).toContain(message);
  });

  it('renders one compact application spine without duplicate authority surfaces', () => {
    const started = performance.now();
    const markup = renderToStaticMarkup(<App />);
    const elapsedMs = performance.now() - started;

    const workspace = markup.indexOf('aria-label="Program authoring workspace"');
    const commands = markup.indexOf('aria-label="3D viewer and live controls"');
    const viewer = markup.indexOf('id="viewer-title"');

    expect(markup).toContain('aria-label="ManyForge Composer"');
    expect(markup).toContain('aria-label="Workbench sections"');
    expect(markup).toContain('>Program</button>');
    expect(markup).toContain('>Scene</button>');
    // Components sits with Program, Scene and Deployment, before Deployment.
    expect(markup).toContain('data-workspace="components"');
    expect(markup.indexOf('data-workspace="scene"')).toBeLessThan(markup.indexOf('data-workspace="components"'));
    expect(markup.indexOf('data-workspace="components"')).toBeLessThan(markup.indexOf('data-workspace="deployment"'));
    expect(markup).toContain('data-workspace="deployment"');
    expect(markup).toContain('data-backend-connected="false"');
    expect(markup).toContain('Live status feed unavailable');
    expect(markup).toContain('>Deployment</span>');
    expect(markup).toContain('aria-label="Program document controls"');
    expect(markup).toContain('deployment-status-button readiness-waiting');
    expect(workspace).toBeGreaterThan(0);
    expect(commands).toBeGreaterThan(workspace);
    expect(viewer).toBeGreaterThan(commands);
    expect(markup).not.toContain('id="program-runtime-title"');
    expect(markup).toContain('Runtime / history');
    expect(markup).toContain('>AI</button>');
    expect(markup).not.toContain('class="live-controls-scroll"');
    expect(markup).not.toContain('Catalog and artifact library');
    expect(markup).not.toContain('Artifacts · Program / Deployment');
    expect(markup).not.toContain('Headless runtime truth is connected');
    expect(markup).not.toContain('class="app-footer"');
    expect(markup.match(/data-workspace="deployment"/g)).toHaveLength(1);
    expect(markup).not.toContain('runtime-status-overlay');
    expect(markup.match(/role="separator"/g)).toHaveLength(3);
    expect(elapsedMs).toBeLessThan(2000);
  });

  it('keeps the canvas fixed and gives scrolling to bounded side surfaces', () => {
    const markup = renderToStaticMarkup(<App />);
    const css = readStyleSource();
    const source = readFileSync(new URL('./App.tsx', import.meta.url), 'utf8');
    const workspaceSource = readFileSync(
      new URL('./components/ProgramWorkspace.tsx', import.meta.url),
      'utf8',
    );
    const sceneSource = readFileSync(
      new URL('./components/SceneAuthoringWorkspace.tsx', import.meta.url),
      'utf8',
    );
    const treeSource = readFileSync(
      new URL('./components/ProgramTreeCanvas.tsx', import.meta.url),
      'utf8',
    );
    const dividerSource = readFileSync(
      new URL('./components/PaneDivider.tsx', import.meta.url),
      'utf8',
    );
    const overlaySource = readFileSync(
      new URL('./components/OverlayPanel.tsx', import.meta.url),
      'utf8',
    );

    expect(markup).not.toContain('aria-label="Collapse viewer and operations pane"');
    expect(markup).not.toContain('aria-label="Expand Operations pane"');
    expect(markup).toContain('aria-label="Resize central inspector (viewer side) pane"');
    expect(markup).toContain('aria-label="Resize central inspector pane"');
    expect(markup).toContain('aria-label="Resize Program tools pane"');
    expect(markup).toContain('aria-label="Collapse node palette"');
    expect(workspaceSource).toContain('Minimize Program inspector');
    expect(workspaceSource).toContain('Maximize Program inspector');
    expect(workspaceSource).toContain('<header className="central-inspector-header">');
    expect(workspaceSource).not.toContain('program-inspector-tabs');
    expect(source).toContain('viewer-inspector-slot');
    expect(source).not.toContain('central-inspector-slot viewer-slot');
    expect(source).toContain('setViewerInspectorMinimized(!alreadyMaximized)');
    expect(source).toContain('setProgramInspectorMinimized(!alreadyMaximized)');
    expect(source).toContain('axis="y"');
    expect(source).toContain('className="central-inspector-divider"');
    expect(css).toContain('.program-inspector-scroll');
    expect(css).toContain('.program-palette-list');
    expect(css).toContain('.program-tree-react-flow');
    expect(css).toMatch(/\.program-tree-react-flow\s*\{[\s\S]*?height: 100%;[\s\S]*?min-height: 0;/);
    expect(css).toMatch(/\.program-inspector-scroll\s*\{[\s\S]*?overflow-y: auto;/);
    expect(css).toContain('.workbench-inspector');
    expect(css).toContain('.central-inspector-slot');
    // The inspector stays centered: editor and viewer share the rest equally.
    expect(source).toContain('scale={2}');
    expect(css).toMatch(/minmax\(0, var\(--inspector-width\)\)\s*0\.45rem\s*minmax\(0, 1fr\)/);
    expect(css).toMatch(/\.viewer-transport\s*\{[\s\S]*?display: grid;/);
    expect(css).toMatch(
      /\.viewer-transport\s*\{[\s\S]*?width: min\(58rem, calc\(100% - 2 \* 0\.45rem\)\);/,
    );
    expect(css).toMatch(/\.transport-control-layout\s*\{[\s\S]*?justify-content: space-between;/);
    expect(css).toContain('--inspector-control-height: 1.9rem');
    expect(css).toContain('.central-inspector-header > strong');
    expect(css).toContain("input:not([type='checkbox']):not([type='range'])");
    expect(source).toContain('<PaneDivider');
    expect(dividerSource).toContain('role="separator"');
    expect(dividerSource).toContain('setPointerCapture');
    expect(dividerSource).toContain("axis === 'x' ? 'ArrowLeft' : 'ArrowUp'");
    expect(dividerSource).toContain("axis === 'x' ? 'ArrowRight' : 'ArrowDown'");
    expect(dividerSource).toContain("'ArrowUp'");
    expect(dividerSource).toContain("'ArrowDown'");
    expect(dividerSource).toContain("event.key === 'Home'");
    expect(dividerSource).toContain("event.key === 'End'");
    expect(overlaySource).toContain("event.key !== 'Escape'");
    expect(overlaySource).toContain('returnFocusRef?.current?.focus()');
    expect(overlaySource).toMatch(/event\.preventDefault\(\);\s*close\(\);/);
    expect(treeSource).toContain('<ProgramNoticeRail');
    expect(workspaceSource).toContain('PROGRAM_INFO_NOTICE_TIMEOUT_MS = 4_000');
    expect(workspaceSource).toContain('onDismissAllNotices={dismissAllNotices}');
    expect(source.match(/<SceneAuthoringWorkspace/g)).toHaveLength(1);
    expect(source.match(/useHeadlessStateSource\(headlessEnabled\)/g)).toHaveLength(1);
    expect(source).toContain('runtimeEnabled={connectedProjection}');
    expect(source).not.toContain('runtimeEnabled={headlessEnabled}');
    expect(sceneSource.match(/<StructuralViewer/g)).toHaveLength(1);
    expect(sceneSource).toContain('createPortal(editorSurface, editorTarget)');
    expect(workspaceSource).toContain("'params'");
    expect(workspaceSource).toContain('<ProgramDeclarationsNavigator');
    expect(workspaceSource).toContain('<ProgramDeclarationsEditor');
    expect(source).not.toContain('<CatalogPanel');
    expect(source).not.toContain('<ArtifactBrowser');
    expect(source).not.toContain('<UnavailableBanner');
    expect(source).not.toContain('type="range"');
  });

  it('shows a submission outcome in the notice rail, never as an extra header slot', () => {
    const appSource = readFileSync(new URL('./App.tsx', import.meta.url), 'utf8');
    const header = appSource.slice(
      appSource.indexOf('<header className="app-header">'),
      appSource.indexOf('</header>'),
    );
    // The header grid has four fixed slots; a fifth child wraps and hides
    // the document actions.
    expect(header).not.toContain('Submission');
    expect(appSource).toContain("title: 'Submission unconfirmed'");
    expect(appSource).toContain("title: 'Submission verified'");
    expect(appSource).toContain('...submissionNotices,');
    // A verified outcome is brief Activity, like other results: no action.
    expect(appSource).toContain("id: 'authoring.submission.verified'");
    expect(appSource).not.toContain('dismissRecheckMessage');
  });

  it('counts warnings apart from errors, never auto-opens for them, and clears them at Reset and Close', () => {
    const appSource = readFileSync(new URL('./App.tsx', import.meta.url), 'utf8');
    expect(appSource).toContain('<span className="global-attention-count warning" title="Warnings">');
    expect(appSource).toContain('{warningNotices.length}');
    const autoOpen = appSource.slice(
      appSource.indexOf('const runtimeAttentionItems'),
      appSource.indexOf('const openRuntimeAttention'),
    );
    expect(autoOpen).toContain('errorConditions.map');
    expect(autoOpen).toContain('blockingAlarms(headlessSnapshot?.alarms)');
    expect(autoOpen).not.toMatch(/clientErrors|warning/i);
    expect(appSource).toContain('if (resetReachedIdle(previous, cycleStateNow)) setWarningClearEpoch(');
    expect(appSource).toContain('if (closed !== null) setWarningClearEpoch(');
    // The red row holds only conditions and blocking alarms.
    expect(appSource).not.toContain('clientErrors={clientErrors}');
  });

  it('opens global rules on Deployment Cell settings, where the rules section mounts and reveals itself', () => {
    const appSource = readFileSync(new URL('./App.tsx', import.meta.url), 'utf8');
    const editorSource = readFileSync(new URL('./components/DeploymentFocusedEditor.tsx', import.meta.url), 'utf8');
    const handler = appSource.slice(
      appSource.indexOf('useEffect(() => onOpenGlobalRulesRequest('),
      appSource.indexOf('[openDeploymentTab, programInvalidEdit]);'),
    );
    expect(handler).toContain("else openDeploymentTab('cell');");
    // Refused, like the Deployment button, while a Program field edit is invalid.
    expect(handler).toContain('if (programInvalidEdit) takeOpenGlobalRulesRequest();');
    expect(handler).not.toContain("setActiveWorkspace('deployment')");
    // The rules mount only on Cell settings and take the pending request when they mount.
    expect(editorSource).toMatch(/\{intent && cell && \(\s*<GlobalInterruptRulesSection/);
    expect(editorSource).toMatch(/if \(!takeOpenGlobalRulesRequest\(\) \|\| section\.current === null\) return;\s*section\.current\.open = true;/);
  });

  it('classifies robot-control errors by type and hides a failed Apply notice only when nothing is held', () => {
    const appSource = readFileSync(new URL('./App.tsx', import.meta.url), 'utf8');
    // Item 19 extended: no message-text classification of what reaches the warning row.
    expect(appSource).toContain('robotControl.state.errorLocal ? null : robotControl.state.error');
    expect(appSource).not.toMatch(/state\.error\.startsWith\(/);
    // Item 21: hidden only on positive evidence (pre-fence refusal, or no failed Apply).
    const notices = appSource.slice(
      appSource.indexOf('const activationNotices = useMemo'),
      appSource.indexOf('const activationCompleted = useBriefNotice'),
    );
    expect(notices).toContain('if (dismissedActivationNotice === id) return [];');
    expect(notices).toContain('outcomeUnknown: applyAcknowledgementOutcomeUnknown,');
    expect(notices).toContain('dismiss: () => setDismissedActivationNotice(id),');
    const handler = appSource.slice(
      appSource.indexOf('const acknowledgeFailedApply = useCallback'),
      appSource.indexOf('const acknowledgingApply ='),
    );
    expect(handler).toContain('await activation.acknowledgeFailure();');
    expect(handler).toMatch(/if \(result\.noFailedApply\) \{\s*setDismissedActivationNotice\(`activation\.\$\{acknowledgementKey\}`\);/);
  });

  it('keeps document, deployment, status, and artifact workflows in their owned locations', () => {
    const appSource = readFileSync(new URL('./App.tsx', import.meta.url), 'utf8');
    const controlsSource = readFileSync(
      new URL('./components/ScenarioActionControls.tsx', import.meta.url),
      'utf8',
    );

    // One header component type for every tab: switching tabs never remounts
    // the Scenario actions or an Apply/Revert confirmation in progress.
    expect(appSource).toContain('<DocumentControls');
    expect(appSource).not.toContain('<ProgramDocumentControls');
    expect(appSource).not.toContain('<DeploymentDocumentControls');
    expect(appSource).toContain('<DeploymentCompositionPanel');
    expect(appSource).toContain('<ComponentsWorkspace');
    // One component catalog for both; Components edits the Deployment Draft, so its header is Deployment's.
    expect(appSource.match(/useComponentCatalog\(/g)).toHaveLength(1);
    expect(appSource).toContain("activeWorkspace === 'deployment' || activeWorkspace === 'components'");
    // Robot Control stays in the viewer: a Components page only opens it.
    expect(appSource).toContain("requestOpenSettings('robot')");
    expect(appSource).toContain('viewerModelRefreshKey(headlessSnapshot)');
    expect(appSource).toMatch(
      /refreshViewerModelsOnManagerChange\([\s\S]*?retrySourceModel,\s*retryManifest,/,
    );
    expect(appSource).toMatch(
      /refreshOnManagerActivationSettled\([\s\S]*?managerActivationSettlementKey\(headlessSnapshot\),\s*\[retryCatalogTracked, refreshDeploymentWorkspace\],/,
    );
    expect(appSource.match(/retryableWhen: isTransientManagerReadinessError/g)).toHaveLength(2);
    expect(appSource).toContain('retryCatalogTracked');
    expect(appSource).toContain('retryNodeEnvironmentTracked');
    expect(appSource).toContain('managerReadinessTransientRetryKey(headlessSnapshot, {');
    expect(appSource).toContain('refreshTransientResourcesOnManagerReadiness(');
    expect(appSource).toContain("catalog.status === 'error' && catalog.retryable === true");
    expect(appSource).toContain("nodeEnvironment.status === 'error' && nodeEnvironment.retryable === true");
    expect(appSource).toContain('runtimeStatusPresentation({');
    expect(appSource).toContain('className={`deployment-status-button readiness-');
    expect(appSource).not.toContain('<RuntimeStatusOverlay');
    expect(appSource).toContain('error: headlessState.error');
    expect(appSource).not.toContain("source: 'composer.headless_projection'");
    expect(appSource).toContain('expanded={runtimePanelExpanded}');
    expect(appSource).toMatch(
      /const openRuntimeAttention[\s\S]*if \(globalPanel !== 'runtime'\)[\s\S]*setRuntimePanelExpanded\(false\)[\s\S]*setGlobalPanel\('runtime'\)/,
    );
    expect(appSource).not.toContain('panelClassName="runtime-observability-overlay"');
    expect(controlsSource).not.toContain('<ArtifactBrowser');
    expect(appSource).toContain('<ScenarioDocumentCommands');
    expect(controlsSource).toContain('Its Deployment will not change.');
    expect(controlsSource).not.toContain('Update Draft');
  });
});

import { useComposerSettings } from '../hooks/useComposerSettings';
import { PROGRAM_TOOLS_WIDTH_PX } from '../state/composerSettings';
import { EDGE_STYLE_OPTIONS, type EdgeStyle, type LayoutDirection } from '../store/composerStore';
import { SettingsSubheading } from './SettingsScope';

/** How the tree editor draws the Program; installation settings. */
export function TreeEditorPreferences() {
  const composerSettings = useComposerSettings();
  const tree = composerSettings.settings.treeEditor;
  const update = (change: Partial<typeof tree>) => composerSettings.update((current) => ({
    ...current,
    treeEditor: { ...current.treeEditor, ...change },
  }));
  return (
    <section className="dcard" aria-label="Tree editor">
      <SettingsSubheading
        title="Tree editor"
        scope="installation"
        onRestoreDefaults={() => composerSettings.restoreSection('treeEditor')}
      />
      <label>
        <span>Link style</span>
        <select
          value={tree.linkStyle}
          onChange={(event) => update({ linkStyle: event.target.value as EdgeStyle })}
        >
          {EDGE_STYLE_OPTIONS.map((option) => (
            <option key={option.value} value={option.value}>{option.label}</option>
          ))}
        </select>
      </label>
      <label>
        <span>Start in layout</span>
        <select
          value={tree.layoutDirection}
          onChange={(event) => update({ layoutDirection: event.target.value as LayoutDirection })}
        >
          <option value="vertical">Vertical</option>
          <option value="horizontal">Horizontal</option>
        </select>
      </label>
      <label title="The width the Nodes / Params pane opens at. Dragging the pane's divider changes it for this session only.">
        <span>Tools pane width</span>
        <span className="dunit">
          <input
            type="range"
            aria-label="Program tools pane default width"
            min={PROGRAM_TOOLS_WIDTH_PX.min}
            max={PROGRAM_TOOLS_WIDTH_PX.max}
            step={4}
            value={tree.toolsWidthPx}
            onChange={(event) => update({ toolsWidthPx: Math.round(Number(event.target.value)) })}
          />
          <output>{tree.toolsWidthPx} px</output>
        </span>
      </label>
      <p>
        The direction and tools pane width the tree editor opens with.
        Dragging the pane divider changes the width for this session only.
        The toolbar&apos;s direction and Auto-align buttons re-lay out the node
        positions stored in the Program: they are Program edits for this
        session, not settings.
      </p>
    </section>
  );
}

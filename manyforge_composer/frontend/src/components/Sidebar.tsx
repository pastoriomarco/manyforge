import { useEffect, useState } from 'react';
import { useCatalogStore } from '../store/catalogStore';
import { NodePaletteItem } from './NodePaletteItem';
import { useComposerStore } from '../store/composerStore';
import { DeploymentPanel } from './DeploymentPanel';
import { ProgramPanel } from './ProgramPanel';
import { SceneDraftMirror, ScenePanel } from './ScenePanel';
import { useProgramStore } from '../store/programStore';
import { profileSatisfies } from '../utils/profiles';

function NodePaletteContent() {
  const { loading, error, fetchCatalog, getByCategory } = useCatalogStore();
  const { mode } = useComposerStore();
  const programProfile = useProgramStore((state) => state.programInfo?.programProfile ?? null);
  const [collapsed, setCollapsed] = useState<Record<string, boolean>>({});

  useEffect(() => {
    fetchCatalog();
  }, [fetchCatalog]);

  const toggleCategory = (cat: string) => {
    setCollapsed(prev => ({ ...prev, [cat]: !prev[cat] }));
  };

  const grouped = Object.fromEntries(
    Object.entries(getByCategory()).map(([category, items]) => [
      category,
      programProfile == null
        ? items
        : items.filter((entry) => profileSatisfies(entry.profilesMin, programProfile)),
    ]),
  );
  const isRunMode = mode === 'run';

  return (
    <div className="flex-1 overflow-y-auto">
      <div className="px-4 py-3 border-b border-gray-700">
        <h2 className="text-sm font-semibold text-gray-400 uppercase tracking-wide">
          Node Palette
        </h2>
        {isRunMode && (
          <p className="text-xs text-yellow-500 mt-1">Editing disabled while running or stopping</p>
        )}
        {programProfile && (
          <p className="text-xs text-gray-500 mt-1">Profile filter: {programProfile}</p>
        )}
      </div>

      <div className="p-2">
        {loading && <p className="text-gray-500 text-sm px-2">Loading catalog...</p>}
        {error && <p className="text-red-400 text-sm px-2">{error}</p>}

        {Object.entries(grouped).map(([category, items]) => (
          <div key={category} className="mb-2">
            <button
              onClick={() => toggleCategory(category)}
              className="w-full flex items-center gap-1 px-2 py-1 text-xs font-semibold text-gray-400 uppercase tracking-wide hover:text-gray-300"
            >
              <span className={`transition-transform ${collapsed[category] ? '' : 'rotate-90'}`}>
                ▶
              </span>
              {category}
              <span className="ml-auto text-gray-600">{items.length}</span>
            </button>

            {!collapsed[category] && (
              <div className="ml-1">
                {items.map(entry => (
                  <NodePaletteItem key={entry.id} entry={entry} disabled={isRunMode} />
                ))}
              </div>
            )}
          </div>
        ))}
      </div>
    </div>
  );
}

export function Sidebar({ onCollapse }: { onCollapse: () => void }) {
  const [tab, setTab] = useState<'nodes' | 'params' | 'scene' | 'deployment'>('nodes');

  return (
    <div className="flex h-full w-full flex-col overflow-hidden bg-gray-800">
      <SceneDraftMirror />
      <div className="flex items-center border-b border-gray-700">
        <button
          onClick={() => setTab('nodes')}
          className={`flex-1 px-3 py-2 text-xs font-semibold uppercase tracking-wide ${
            tab === 'nodes'
              ? 'text-white border-b-2 border-blue-500'
              : 'text-gray-500 hover:text-gray-300'
          }`}
        >
          Nodes
        </button>
        <button
          onClick={() => setTab('params')}
          className={`flex-1 px-3 py-2 text-xs font-semibold uppercase tracking-wide ${
            tab === 'params'
              ? 'text-white border-b-2 border-blue-500'
              : 'text-gray-500 hover:text-gray-300'
          }`}
        >
          Params
        </button>
        <button
          onClick={() => setTab('scene')}
          className={`flex-1 px-3 py-2 text-xs font-semibold uppercase tracking-wide ${
            tab === 'scene'
              ? 'text-white border-b-2 border-blue-500'
              : 'text-gray-500 hover:text-gray-300'
          }`}
        >
          Scene
        </button>
        <button
          onClick={() => setTab('deployment')}
          className={`flex-1 px-3 py-2 text-xs font-semibold uppercase tracking-wide ${
            tab === 'deployment'
              ? 'text-white border-b-2 border-blue-500'
              : 'text-gray-500 hover:text-gray-300'
          }`}
        >
          Deployment
        </button>
        <button
          type="button"
          onClick={onCollapse}
          className="shrink-0 px-2 py-2 text-lg leading-none text-gray-400 hover:text-white"
          title="Collapse sidebar"
        >
          ‹
        </button>
      </div>

      {tab === 'nodes'
        ? <NodePaletteContent />
        : tab === 'params'
          ? <ProgramPanel />
          : tab === 'scene'
            ? <ScenePanel />
            : <DeploymentPanel />}
    </div>
  );
}

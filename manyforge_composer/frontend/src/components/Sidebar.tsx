import { useEffect, useState } from 'react';
import { useCatalogStore } from '../store/catalogStore';
import { NodePaletteItem } from './NodePaletteItem';
import { useComposerStore } from '../store/composerStore';
import { DeploymentPanel } from './DeploymentPanel';

function NodePaletteContent() {
  const { loading, error, fetchCatalog, getByCategory } = useCatalogStore();
  const { mode } = useComposerStore();
  const [collapsed, setCollapsed] = useState<Record<string, boolean>>({});

  useEffect(() => {
    fetchCatalog();
  }, [fetchCatalog]);

  const toggleCategory = (cat: string) => {
    setCollapsed(prev => ({ ...prev, [cat]: !prev[cat] }));
  };

  const grouped = getByCategory();
  const isRunMode = mode === 'run';

  return (
    <div className="flex-1 overflow-y-auto">
      <div className="px-4 py-3 border-b border-gray-700">
        <h2 className="text-sm font-semibold text-gray-400 uppercase tracking-wide">
          Node Palette
        </h2>
        {isRunMode && (
          <p className="text-xs text-yellow-500 mt-1">Editing disabled while running</p>
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

export function Sidebar() {
  const [tab, setTab] = useState<'nodes' | 'deployment'>('nodes');

  return (
    <div className="w-60 bg-gray-800 border-r border-gray-700 flex flex-col overflow-hidden">
      <div className="flex border-b border-gray-700">
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
          onClick={() => setTab('deployment')}
          className={`flex-1 px-3 py-2 text-xs font-semibold uppercase tracking-wide ${
            tab === 'deployment'
              ? 'text-white border-b-2 border-blue-500'
              : 'text-gray-500 hover:text-gray-300'
          }`}
        >
          Deployment
        </button>
      </div>

      {tab === 'nodes' ? <NodePaletteContent /> : <DeploymentPanel />}
    </div>
  );
}

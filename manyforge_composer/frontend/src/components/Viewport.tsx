import { TreeCanvas } from './TreeCanvas';
import { PropertyPanel } from './PropertyPanel';

export function Viewport() {
  return (
    <div className="flex-1 flex overflow-hidden">
      <TreeCanvas />
      <PropertyPanel />
    </div>
  );
}

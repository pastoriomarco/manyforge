import { Sidebar } from './Sidebar';
import { Viewport } from './Viewport';
import { ControlBar } from './ControlBar';

export function Layout() {
  return (
    <div className="h-screen flex flex-col bg-gray-900 text-white">
      <div className="flex flex-1 overflow-hidden">
        <Sidebar />
        <Viewport />
      </div>
      <ControlBar />
    </div>
  );
}

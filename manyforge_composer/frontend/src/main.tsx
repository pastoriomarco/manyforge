import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import './index.css';
import { seedCameraFromUrl } from './store/viewerSelectionStore';

// Read `?cam=x,y,z&target=x,y,z` once and stash it for the Scene
// Viewer's first mount to consume. Day 4 contract item — programmatic
// camera control for "frame to selection" and similar features will
// reuse the same store action.
seedCameraFromUrl();

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>
);

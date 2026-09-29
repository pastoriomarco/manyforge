import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import './styles/index.css';
import { OperationalErrorProvider } from './state/operationalErrors';
import { WorkbenchNoticeProvider } from './state/workbenchNotices';
import { ComposerSettingsProvider } from './hooks/useComposerSettings';

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <OperationalErrorProvider>
      <WorkbenchNoticeProvider>
        <ComposerSettingsProvider>
          <App />
        </ComposerSettingsProvider>
      </WorkbenchNoticeProvider>
    </OperationalErrorProvider>
  </React.StrictMode>,
);
